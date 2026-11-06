// OpenMPC Host: a VST3 that runs an unchanged MPC plugin (.so, armv7 or aarch64) under the emulator (poc/mpc_host).
// One object is IComponent + IAudioProcessor + IEditController. Design: docs/MPC_VST3_HOST.md. M1: instrument, runtime parameters,
// Q-Link section (4 or 8 knobs, bank, next-bank button), audio resampled to the host rate, MIDI, state. No editor yet.
// The ABI is hand-written (mpc-vst-plugins/wrapper/vst3_abi.h); no Steinberg SDK.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define _Static_assert static_assert   // the ABI header is C
#include "../mpc-vst-plugins/wrapper/vst3_abi.h"
#include "../mpc-vst-plugins/wrapper/resample.h"
#include "../poc/mpc_host/hosted.h"

using mpchost::HostedPlugin;

#define VST3_EXPORT_NAME "OpenMPC Host"
enum : uint32_t {
    kQLink1 = 1,          // 1..8
    kQBank = 9, kQNext = 10, kQMode = 11,
    kBypass = 12,
    kPluginBase = 100,
    kMidiBase = 100000,   // + channel*130 + controller (0..127 CC, 128 channel pressure, 129 pitch bend): hidden, for IMidiMapping    // 100 + hosted parameter index
};
static const uint8_t g_cid[16] = {0x4f, 0x70, 0x65, 0x6e, 0x4d, 0x50, 0x43, 0x48, 0x6f, 0x73, 0x74, 0x56, 0x53, 0x54, 0x33, 0x01};

// ---- module location ------------------------------------------------------------------------------------------------
static std::string module_dir() {   // the folder holding the .vst3 binary
    std::string p;
#ifdef _WIN32
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&module_dir, &m);
    char b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(m, b, sizeof b);
    p.assign(b, n);
    for (auto& c : p) if (c == '\\') c = '/';
#else
    Dl_info di;
    if (dladdr((void*)&module_dir, &di) && di.dli_fname) p = di.dli_fname;
#endif
    size_t s = p.rfind('/');
    return s == std::string::npos ? "." : p.substr(0, s);
}
static std::string user_dir() {   // where the hosted plugins' "/" lives (their settings, downloads, the manager's installs)
    if (const char* e = std::getenv("OPENMPC_HOST_ROOT")) return e;
#ifdef _WIN32
    const char* a = std::getenv("APPDATA");
    return std::string(a ? a : ".") + "/OpenMPC Host/root";
#else
    const char* h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.local/share/openmpc-host/root";
#endif
}
static bool has(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }

// The plugin to host: OPENMPC_HOST_PLUGIN (a .so path), else Resources/default.txt (a folder name under Resources/plugins/), else the manager.
static std::string find_plugin(std::string& dir) {
    if (const char* e = std::getenv("OPENMPC_HOST_PLUGIN")) { std::string s = e; size_t k = s.rfind('/'); dir = k == std::string::npos ? "." : s.substr(0, k); return s; }
    const std::string res = module_dir() + "/../Resources";
    std::string name = "openmpc_pm_g2";
    if (FILE* f = fopen((res + "/default.txt").c_str(), "rb")) { char b[256] = {}; if (fgets(b, sizeof b, f)) { name = b; while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back(); } fclose(f); }
    dir = res + "/plugins/" + name;
    return dir + "/" + name + ".so";
}

static void to16(char16* d, const std::string& s, int cap = 128) {
    int i = 0;
    for (; i < cap - 1 && i < (int)s.size(); i++) d[i] = (unsigned char)s[i];
    d[i] = 0;
}
static std::string from16(const char16* s) { std::string r; for (; *s; s++) r += *s < 128 ? (char)*s : '?'; return r; }
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---- the object -------------------------------------------------------------------------------------------------------
struct MidiEv { int frame; uint8_t m[3]; };
struct Host {
    const ComponentVtbl* comp; const ProcessorVtbl* proc; const ControllerVtbl* ctrl; const MidiMappingVtbl* midimap; const CtxReqVtbl* ctxreq;
    uint32_t refs = 1;
    std::mutex mu;                    // the emulator is one CPU: one call at a time
    HostedPlugin* hp = nullptr;
    std::string load_error;
    IComponentHandler* handler = nullptr;
    int nparams = 0;
    std::vector<float> pv;            // the host's view of each hosted parameter
    int qmode = 4, qbank = 0;         // Q-Link section: 4 or 8 knobs, and the bank shown
    double qval[8] = {};              // the knobs' own normalised values (what the host sees)
    double rate = 44100;
    rs_t* rs = nullptr;
    std::vector<float> ofl, ofr; int olen = 0;
    std::vector<MidiEv> ev;
    double next_last = 0; bool bypass = false;
    double bpm = 120, ppq = 0; bool playing = false;
    std::vector<uint32_t> out_ids; std::vector<double> out_vals;   // plugin-side changes to report to the host
};
#define FROM(p, m) ((Host*)((char*)(p) - offsetof(Host, m)))

static int nbanks(Host* h) { int n = (h->nparams + h->qmode - 1) / h->qmode; return n < 1 ? 1 : n > 16 ? 16 : n; }
// Fallback Q-Link map (until the skin's Q-Link sets are read): knob n of bank b drives hosted parameter b*mode+n.
static int qparam(Host* h, int knob) { int p = h->qbank * h->qmode + knob; return p < h->nparams ? p : -1; }
static void qrefresh(Host* h) {   // knobs show their parameter's value after a bank or mode change
    for (int k = 0; k < 8; k++) { int p = k < h->qmode ? qparam(h, k) : -1; h->qval[k] = p >= 0 ? h->pv[p] : 0; }
}
static void set_hosted(Host* h, int p, double v) {   // callers hold mu
    if (p < 0 || p >= h->nparams) return;
    h->pv[p] = (float)v;
    h->hp->set_param(p, (float)v);
    for (int k = 0; k < h->qmode; k++) if (qparam(h, k) == p) h->qval[k] = v;
}

static bool load(Host* h) {   // callers hold mu
    if (h->hp) return true;
    std::string dir, so = find_plugin(dir);
    if (!has(so)) { h->load_error = "plugin not found: " + so; return false; }
    auto* hp = new HostedPlugin();
    HostedPlugin::Config cfg;
    cfg.root = user_dir();
    cfg.support_dir = module_dir() + "/../Resources";
    if (!hp->open(so, cfg, h->load_error)) { delete hp; return false; }
    hp->start();
    h->hp = hp;
    h->nparams = hp->num_params();
    h->pv.assign(h->nparams, 0);
    for (int i = 0; i < h->nparams; i++) h->pv[i] = hp->get_param(i);
    qrefresh(h);
    return true;
}

static void rs_reset(Host* h) {
    if (h->rs) { rs_free(h->rs); delete h->rs; h->rs = nullptr; }
    h->rs = new rs_t;
    rs_init(h->rs, HostedPlugin::kRate, h->rate);
    h->ofl.assign(16384 + 4096, 0); h->ofr.assign(16384 + 4096, 0); h->olen = 0;
}

// ---- IUnknown ---------------------------------------------------------------------------------------------------------
static bool iid_eq(const void* a, const TUID b) { return !memcmp(a, b, 16); }
static tresult query(Host* h, const uint8_t* iid, void** obj) {
    if (iid_eq(iid, iid_FUnknown) || iid_eq(iid, iid_IComponent) || iid_eq(iid, iid_IPluginBase)) *obj = &h->comp;
    else if (iid_eq(iid, iid_IAudioProcessor)) *obj = &h->proc;
    else if (iid_eq(iid, iid_IEditController)) *obj = &h->ctrl;
    else if (iid_eq(iid, iid_IMidiMapping)) *obj = &h->midimap;
    else if (iid_eq(iid, iid_IProcessContextRequirements)) *obj = &h->ctxreq;
    else { *obj = nullptr; return kNoInterface; }
    h->refs++;
    return kResultOk;
}
static uint32_t release(Host* h) {
    if (--h->refs) return h->refs;
    if (h->handler) h->handler->v->release(h->handler);
    if (h->rs) { rs_free(h->rs); delete h->rs; }
    delete h->hp;
    delete h;
    return 0;
}
#define IFACE_UNKNOWN(NAME, MEMBER) \
    static tresult NAME##_query(void* s, const TUID iid, void** o) { return query(FROM(s, MEMBER), iid, o); } \
    static uint32_t NAME##_ref(void* s) { return ++FROM(s, MEMBER)->refs; } \
    static uint32_t NAME##_rel(void* s) { return release(FROM(s, MEMBER)); }
IFACE_UNKNOWN(comp, comp) IFACE_UNKNOWN(proc, proc) IFACE_UNKNOWN(ctrl, ctrl) IFACE_UNKNOWN(mm, midimap) IFACE_UNKNOWN(cr, ctxreq)

// ---- IComponent -------------------------------------------------------------------------------------------------------
static tresult comp_init(void* s, FUnknown*) { Host* h = FROM(s, comp); std::lock_guard<std::mutex> l(h->mu); load(h); return kResultOk; }
static tresult comp_term(void*) { return kResultOk; }
static tresult comp_ctrl_id(void*, TUID) { return kNotImplemented; }   // one object: the component is also the controller
static tresult comp_io(void*, int32_t) { return kResultOk; }
static int32_t comp_bus_count(void*, int32_t type, int32_t dir) { return type == kAudio ? (dir == kOutput ? 1 : 0) : (dir == kInput ? 1 : 0); }
static tresult comp_bus_info(void*, int32_t type, int32_t dir, int32_t i, BusInfo* b) {
    if (i != 0 || comp_bus_count(nullptr, type, dir) == 0) return kInvalidArgument;
    memset(b, 0, sizeof *b);
    b->mediaType = type; b->direction = dir; b->channelCount = type == kAudio ? 2 : 1;
    to16(b->name, type == kAudio ? "Out" : "MIDI In");
    b->busType = kMain; b->flags = kDefaultActive;
    return kResultOk;
}
static tresult comp_routing(void*, RoutingInfo*, RoutingInfo*) { return kNotImplemented; }
static tresult comp_activate_bus(void*, int32_t, int32_t, int32_t, uint8_t) { return kResultOk; }
static tresult comp_set_active(void* s, uint8_t on) {
    Host* h = FROM(s, comp);
    std::lock_guard<std::mutex> l(h->mu);
    if (on && !h->rs) rs_reset(h);
    return kResultOk;
}

static void put(IBStream* st, const void* p, int n) { int32_t d = 0; st->v->write(st, (void*)p, n, &d); }
static bool get(IBStream* st, void* p, int n) { int32_t d = 0; return st->v->read(st, p, n, &d) == kResultOk && d == n; }
static tresult comp_get_state(void* s, IBStream* st) {
    Host* h = FROM(s, comp);
    std::lock_guard<std::mutex> l(h->mu);
    std::vector<uint8_t> chunk = h->hp ? h->hp->get_chunk() : std::vector<uint8_t>();
    uint32_t magic = 0x3148504f /* "OPH1" */, n = (uint32_t)chunk.size(), np = (uint32_t)h->pv.size();
    uint8_t q[2] = {(uint8_t)h->qmode, (uint8_t)h->qbank};
    put(st, &magic, 4); put(st, q, 2); put(st, &n, 4); if (n) put(st, chunk.data(), (int)n);
    put(st, &np, 4); if (np) put(st, h->pv.data(), (int)(np * 4));
    return kResultOk;
}
static tresult comp_set_state(void* s, IBStream* st) {
    Host* h = FROM(s, comp);
    std::lock_guard<std::mutex> l(h->mu);
    uint32_t magic = 0, n = 0, np = 0;
    uint8_t q[2];
    if (!get(st, &magic, 4) || magic != 0x3148504f || !get(st, q, 2) || !get(st, &n, 4) || n > (64u << 20)) return kResultFalse;
    std::vector<uint8_t> chunk(n);
    if (n && !get(st, chunk.data(), (int)n)) return kResultFalse;
    h->qmode = q[0] == 8 ? 8 : 4; h->qbank = q[1];
    if (h->hp && n) {
        h->hp->set_chunk(chunk);
        for (int i = 0; i < h->nparams; i++) h->pv[i] = h->hp->get_param(i);
    } else if (get(st, &np, 4) && h->hp) {
        std::vector<float> v(np);
        if (np && get(st, v.data(), (int)(np * 4))) for (uint32_t i = 0; i < np && (int)i < h->nparams; i++) set_hosted(h, (int)i, v[i]);
    }
    if (h->qbank >= nbanks(h)) h->qbank = 0;
    qrefresh(h);
    return kResultOk;
}
static const ComponentVtbl comp_vtbl = {comp_query, comp_ref, comp_rel, comp_init, comp_term, comp_ctrl_id, comp_io, comp_bus_count, comp_bus_info,
                                        comp_routing, comp_activate_bus, comp_set_active, comp_set_state, comp_get_state};

// ---- IAudioProcessor --------------------------------------------------------------------------------------------------
static tresult proc_set_arr(void*, uint64_t*, int32_t ni, uint64_t* out, int32_t no) { return ni == 0 && no == 1 && out[0] == 3 ? kResultOk : kResultFalse; }
static tresult proc_get_arr(void*, int32_t dir, int32_t i, uint64_t* a) { if (dir != kOutput || i) return kInvalidArgument; *a = 3; return kResultOk; }
static tresult proc_can_size(void*, int32_t sz) { return sz == 0 ? kResultOk : kResultFalse; }
static uint32_t proc_latency(void*) { return 0; }
static tresult proc_setup(void* s, ProcessSetup* ps) {
    Host* h = FROM(s, proc);
    std::lock_guard<std::mutex> l(h->mu);
    h->rate = ps->sampleRate > 0 ? ps->sampleRate : 44100;
    rs_reset(h);
    return kResultOk;
}
static tresult proc_set_processing(void*, uint8_t) { return kResultOk; }

static void send(Host* h, const MidiEv& e) { h->hp->midi(e.m[0], e.m[1], e.m[2], 0); }
static void render_block(Host* h) {   // one 44.1 kHz block from the plugin, converted into the output FIFO
    float l[HostedPlugin::kBlock] = {}, r[HostedPlugin::kBlock] = {};
    h->hp->process(l, r);
    rs_push(h->rs, l, r, HostedPlugin::kBlock);
    int room = (int)h->ofl.size() - h->olen;
    if (room > 0) h->olen += rs_pull(h->rs, h->ofl.data() + h->olen, h->ofr.data() + h->olen, room);
}
static void apply_param_change(Host* h, uint32_t id, double v) {   // mu held
    v = clamp01(v);
    if (id >= kQLink1 && id < kQLink1 + 8) {
        int k = (int)(id - kQLink1);
        h->qval[k] = v;
        if (k < h->qmode) set_hosted(h, qparam(h, k), v);
    } else if (id == kQNext) {
        if (v > 0.5) { h->qbank = (h->qbank + 1) % nbanks(h); qrefresh(h); }
    } else if (id == kQBank) {
        int b = (int)lround(v * 15);
        if (b < nbanks(h)) { h->qbank = b; qrefresh(h); }
    } else if (id == kBypass) {
        h->bypass = v > 0.5;
    } else if (id >= kMidiBase) {
        // a controller change from the host: timed MIDI (the caller queues it in process())
    } else if (id == kQMode) {
        h->qmode = v > 0.5 ? 8 : 4;
        if (h->qbank >= nbanks(h)) h->qbank = 0;
        qrefresh(h);
    } else if (id >= kPluginBase && id < kMidiBase) {
        set_hosted(h, (int)(id - kPluginBase), v);
    }
}

static tresult proc_process(void* s, ProcessData* d) {
    Host* h = FROM(s, proc);
    std::lock_guard<std::mutex> lk(h->mu);
    if (!h->hp || !h->rs) return kResultFalse;
    int n = d->numSamples;
    // the Next Bank button reports a press as a rise through 0.5: remember the last value to see the edge
    double& next_last = h->next_last;
    std::vector<MidiEv> pend;
    if (IParameterChanges* pc = d->inputParameterChanges) {
        int np = pc->v->getParameterCount(pc);
        for (int k = 0; k < np; k++) {
            IParamValueQueue* q = pc->v->getParameterData(pc, k);
            if (!q) continue;
            uint32_t id = q->v->getParameterId(q);
            int pts = q->v->getPointCount(q);
            for (int j = 0; j < pts; j++) {
                int32_t off; double v;
                if (q->v->getPoint(q, j, &off, &v) != kResultOk) continue;
                if (id >= kMidiBase) {
                    int k = (int)(id - kMidiBase), ch = k / 130 % 16, c = k % 130;
                    MidiEv m{off, {0, 0, 0}};
                    if (c == 129) { int pb = (int)lround(clamp01(v) * 16383); m.m[0] = 0xE0 | ch; m.m[1] = pb & 127; m.m[2] = pb >> 7; }
                    else if (c == 128) { m.m[0] = 0xD0 | ch; m.m[1] = (uint8_t)lround(clamp01(v) * 127); }
                    else { m.m[0] = 0xB0 | ch; m.m[1] = c; m.m[2] = (uint8_t)lround(clamp01(v) * 127); }
                    pend.push_back(m);
                } else if (id == kQNext) { if (v > 0.5 && next_last <= 0.5) apply_param_change(h, id, v); next_last = v; }
                else apply_param_change(h, id, v);
            }
        }
    }
    h->ev.clear();
    if (IEventList* el = d->inputEvents) {
        int ne = el->v->getEventCount(el);
        for (int k = 0; k < ne; k++) {
            Event e;
            if (el->v->getEvent(el, k, &e) != kResultOk) continue;
            MidiEv m{e.sampleOffset, {0, 0, 0}};
            if (e.type == kNoteOnEvent) { int v = (int)lroundf(e.noteOn.velocity * 127); m.m[0] = 0x90 | (e.noteOn.channel & 15); m.m[1] = e.noteOn.pitch & 127; m.m[2] = v < 1 ? 1 : v > 127 ? 127 : v; }
            else if (e.type == kNoteOffEvent) { m.m[0] = 0x80 | (e.noteOff.channel & 15); m.m[1] = e.noteOff.pitch & 127; m.m[2] = (uint8_t)lroundf((float)clamp01(e.noteOff.velocity) * 127); }
            else if (e.type == kPolyPressureEvent) { m.m[0] = 0xA0 | (e.polyPressure.channel & 15); m.m[1] = e.polyPressure.pitch & 127; m.m[2] = (uint8_t)lroundf((float)clamp01(e.polyPressure.pressure) * 127); }
            else continue;
            h->ev.push_back(m);
        }
    }
    h->ev.insert(h->ev.end(), pend.begin(), pend.end());
    std::stable_sort(h->ev.begin(), h->ev.end(), [](const MidiEv& a, const MidiEv& b) { return a.frame < b.frame; });
    if (ProcessContext* c = d->processContext) {
        h->playing = c->state & kPlaying;
        if (c->state & kTempoValid) h->bpm = c->tempo;
        if (c->state & kProjectTimeMusicValid) h->ppq = c->projectTimeMusic;
        h->hp->set_transport(h->bpm, h->ppq, h->playing);
    }
    float *ol = nullptr, *orr = nullptr;
    if (d->numOutputs > 0 && d->outputs[0].numChannels >= 2) { ol = d->outputs[0].channelBuffers32[0]; orr = d->outputs[0].channelBuffers32[1]; }
    size_t evk = 0;
    int pos = 0;
    while (pos < n) {
        while (evk < h->ev.size() && h->ev[evk].frame <= pos) send(h, h->ev[evk++]);
        int end = evk < h->ev.size() && h->ev[evk].frame < n ? h->ev[evk].frame : n;
        if (end - pos > 512) end = pos + 512;
        while (h->olen < end - pos) render_block(h);
        int m = end - pos;
        if (ol) { memcpy(ol + pos, h->ofl.data(), m * 4); memcpy(orr + pos, h->ofr.data(), m * 4); }
        memmove(h->ofl.data(), h->ofl.data() + m, (h->olen - m) * 4);
        memmove(h->ofr.data(), h->ofr.data() + m, (h->olen - m) * 4);
        h->olen -= m;
        pos = end;
    }
    while (evk < h->ev.size()) send(h, h->ev[evk++]);
    if (ol && h->bypass) { memset(ol, 0, n * 4); memset(orr, 0, n * 4); }
    if (ol) d->outputs[0].silenceFlags = 0;
    // changes the plugin made itself go back to the host
    for (auto& a : h->hp->take_automation()) {
        if (a.first < 0 || a.first >= h->nparams) continue;
        h->pv[a.first] = a.second;
        for (int k = 0; k < h->qmode; k++) if (qparam(h, k) == a.first) h->qval[k] = a.second;
        if (IParameterChanges* oc = d->outputParameterChanges) {
            uint32_t id = kPluginBase + a.first; int32_t idx = 0;
            if (IParamValueQueue* q = oc->v->addParameterData(oc, &id, &idx)) { int32_t pi; q->v->addPoint(q, 0, a.second, &pi); }
        }
    }
    return kResultOk;
}
static uint32_t proc_tail(void*) { return 0; }
static const ProcessorVtbl proc_vtbl = {proc_query, proc_ref, proc_rel, proc_set_arr, proc_get_arr, proc_can_size, proc_latency, proc_setup, proc_set_processing,
                                        proc_process, proc_tail};

// ---- IEditController --------------------------------------------------------------------------------------------------
static tresult ctrl_init(void*, FUnknown*) { return kResultOk; }
static tresult ctrl_term(void* s) { Host* h = FROM(s, ctrl); if (h->handler) { h->handler->v->release(h->handler); h->handler = nullptr; } return kResultOk; }
static tresult ctrl_set_comp_state(void*, IBStream*) { return kResultOk; }   // one object: the component state is already applied
static tresult ctrl_set_state(void*, IBStream*) { return kResultOk; }
static tresult ctrl_get_state(void*, IBStream*) { return kResultOk; }
// Parameters: Q-Link 1..8, Q-Link Bank, Next Bank, Q-Link Mode, then every parameter of the hosted plugin.
static int32_t ctrl_count(void* s) { Host* h = FROM(s, ctrl); std::lock_guard<std::mutex> l(h->mu); return 12 + h->nparams + 16 * 130; }
static uint32_t id_at(int i, int np) { return i < 8 ? kQLink1 + i : i < 11 ? kQBank + (i - 8) : i == 11 ? kBypass : i < 12 + np ? kPluginBase + (i - 12) : kMidiBase + (i - 12 - np); }
static tresult ctrl_info(void* s, int32_t i, ParameterInfo* pi) {
    Host* h = FROM(s, ctrl);
    std::lock_guard<std::mutex> l(h->mu);
    if (i < 0 || i >= 12 + h->nparams + 16 * 130) return kInvalidArgument;
    memset(pi, 0, sizeof *pi);
    pi->id = id_at(i, h->nparams);
    if (pi->id >= kMidiBase) {
        int k = (int)(pi->id - kMidiBase), c = k % 130;
        char nm[40];
        if (c == 128) snprintf(nm, sizeof nm, "Ch %d Pressure", k / 130 + 1);
        else if (c == 129) snprintf(nm, sizeof nm, "Ch %d Pitch Bend", k / 130 + 1);
        else snprintf(nm, sizeof nm, "Ch %d CC %d", k / 130 + 1, c);
        to16(pi->title, nm); to16(pi->shortTitle, nm);
        pi->stepCount = c == 129 ? 0 : 127; pi->defaultNormalizedValue = c == 129 ? 0.5 : 0; pi->flags = kIsHidden | kIsReadOnly;
        return kResultOk;
    }
    if (pi->id == kBypass) { to16(pi->title, "Bypass"); to16(pi->shortTitle, "Bypass"); pi->stepCount = 1; pi->flags = kCanAutomate | kIsBypass; return kResultOk; }
    std::string t;
    if (i < 8) { t = "Q-Link " + std::to_string(i + 1); pi->flags = kCanAutomate; }
    else if (pi->id == kQBank) { t = "Q-Link Bank"; pi->stepCount = 15; pi->flags = kCanAutomate | kIsList; }
    else if (pi->id == kQNext) { t = "Q-Link Next Bank"; pi->stepCount = 1; pi->flags = kCanAutomate; }
    else if (pi->id == kQMode) { t = "Q-Link Mode (4/8)"; pi->stepCount = 1; pi->flags = kCanAutomate | kIsList; }
    else {
        int p = i - 12;
        t = h->hp ? h->hp->param_name(p) : "Param";
        if (t.empty()) t = "Param " + std::to_string(p + 1);
        pi->defaultNormalizedValue = h->pv[p];
        pi->flags = kCanAutomate;
        std::string u = h->hp ? h->hp->param_label(p) : "";
        to16(pi->units, u);
    }
    to16(pi->title, t); to16(pi->shortTitle, t.substr(0, 12));
    return kResultOk;
}
static double ctrl_get(void* s, uint32_t id) {
    Host* h = FROM(s, ctrl);
    std::lock_guard<std::mutex> l(h->mu);
    if (id >= kQLink1 && id < kQLink1 + 8) return h->qval[id - kQLink1];
    if (id == kQBank) return h->qbank / 15.0;
    if (id == kQNext) return 0;
    if (id == kBypass) return h->bypass;
    if (id >= kMidiBase) return (id - kMidiBase) % 130 == 129 ? 0.5 : 0;
    if (id == kQMode) return h->qmode == 8 ? 1 : 0;
    int p = (int)id - (int)kPluginBase;
    return p >= 0 && p < h->nparams ? h->pv[p] : 0;
}
static tresult ctrl_set(void* s, uint32_t id, double v) {
    Host* h = FROM(s, ctrl);
    std::lock_guard<std::mutex> l(h->mu);
    if (id == kQNext) { if (v > 0.5) apply_param_change(h, id, v); return kResultOk; }   // processor side handles automation edges
    apply_param_change(h, id, v);
    return kResultOk;
}
static tresult ctrl_str(void* s, uint32_t id, double v, char16* out) {
    Host* h = FROM(s, ctrl);
    std::lock_guard<std::mutex> l(h->mu);
    char b[64];
    if (id == kQBank) snprintf(b, sizeof b, "Bank %d", (int)lround(v * 15) + 1);
    else if (id == kQMode) snprintf(b, sizeof b, "%s", v > 0.5 ? "8 knobs" : "4 knobs");
    else if (id == kQNext) snprintf(b, sizeof b, "%s", v > 0.5 ? "Next" : "-");
    else {
        int p = id >= kPluginBase ? (int)(id - kPluginBase) : (id >= kQLink1 && id < kQLink1 + 8 && (int)(id - kQLink1) < h->qmode ? qparam(h, (int)(id - kQLink1)) : -1);
        if (p >= 0 && p < h->nparams && h->hp && fabs(v - h->pv[p]) < 1e-6) {
            std::string d = h->hp->param_display(p), u = h->hp->param_label(p);
            snprintf(b, sizeof b, "%s%s%s", d.c_str(), u.empty() ? "" : " ", u.c_str());
        } else snprintf(b, sizeof b, "%.1f %%", v * 100);
    }
    to16(out, b, 128);
    return kResultOk;
}
static tresult ctrl_val(void*, uint32_t, char16* str, double* v) { *v = clamp01(atof(from16(str).c_str()) / 100.0); return kResultOk; }
static double ctrl_n2p(void*, uint32_t, double v) { return v; }
static double ctrl_p2n(void*, uint32_t, double v) { return v; }
static tresult ctrl_handler(void* s, IComponentHandler* hd) {
    Host* h = FROM(s, ctrl);
    if (h->handler) h->handler->v->release(h->handler);
    h->handler = hd;
    if (hd) hd->v->addRef(hd);
    return kResultOk;
}
static void* ctrl_view(void*, const char*) { return nullptr; }   // M3: the skin editor
static const ControllerVtbl ctrl_vtbl = {ctrl_query, ctrl_ref, ctrl_rel, ctrl_init, ctrl_term, ctrl_set_comp_state, ctrl_set_state, ctrl_get_state, ctrl_count, ctrl_info,
                                         ctrl_str, ctrl_val, ctrl_n2p, ctrl_p2n, ctrl_get, ctrl_set, ctrl_handler, ctrl_view};

static tresult mm_assign(void*, int32_t bus, int16_t ch, int16_t ctrl, uint32_t* id) {
    if (bus != 0 || ch < 0 || ch > 15 || ctrl < 0 || ctrl > 129) return kResultFalse;
    *id = kMidiBase + ch * 130 + ctrl;   // 128 = aftertouch, 129 = pitch bend in VST3's controller numbering
    return kResultOk;
}
static const MidiMappingVtbl mm_vtbl = {mm_query, mm_ref, mm_rel, mm_assign};
static uint32_t cr_req(void*) { return 1 | 2; }   // transport state and tempo
static const CtxReqVtbl cr_vtbl = {cr_query, cr_ref, cr_rel, cr_req};

// ---- factory ----------------------------------------------------------------------------------------------------------
static tresult f_query(void*, const TUID iid, void** o) {
    extern const FactoryVtbl* const g_factory_ptr;
    if (iid_eq(iid, iid_FUnknown) || iid_eq(iid, iid_IPluginFactory) || iid_eq(iid, iid_IPluginFactory2)) { *o = (void*)&g_factory_ptr; return kResultOk; }
    *o = nullptr;
    return kNoInterface;
}
static uint32_t f_ref(void*) { return 1; }
static tresult f_info(void*, PFactoryInfo* i) {
    memset(i, 0, sizeof *i);
    snprintf(i->vendor, sizeof i->vendor, "OpenMPC");
    snprintf(i->url, sizeof i->url, "https://github.com/open-mpc/mpc-vst-desktop");
    i->flags = 8;   // unicode
    return kResultOk;
}
static int32_t f_count(void*) { return 1; }
static tresult f_class(void*, int32_t i, PClassInfo* ci) {
    if (i) return kInvalidArgument;
    memset(ci, 0, sizeof *ci);
    memcpy(ci->cid, g_cid, 16); ci->cardinality = 0x7FFFFFFF;
    snprintf(ci->category, sizeof ci->category, "Audio Module Class"); snprintf(ci->name, sizeof ci->name, VST3_EXPORT_NAME);
    return kResultOk;
}
static tresult f_class2(void*, int32_t i, PClassInfo2* ci) {
    if (i) return kInvalidArgument;
    memset(ci, 0, sizeof *ci);
    memcpy(ci->cid, g_cid, 16); ci->cardinality = 0x7FFFFFFF;
    snprintf(ci->category, sizeof ci->category, "Audio Module Class"); snprintf(ci->name, sizeof ci->name, VST3_EXPORT_NAME);
    snprintf(ci->subCategories, sizeof ci->subCategories, "Instrument|Synth");
    snprintf(ci->vendor, sizeof ci->vendor, "OpenMPC"); snprintf(ci->version, sizeof ci->version, "0.1.0"); snprintf(ci->sdkVersion, sizeof ci->sdkVersion, "VST 3.7.0");
    return kResultOk;
}
static tresult f_create(void*, const char* cid, const char* iid, void** obj) {
    *obj = nullptr;
    if (memcmp(cid, g_cid, 16)) return kNoInterface;
    Host* h = new Host();
    h->comp = &comp_vtbl; h->proc = &proc_vtbl; h->ctrl = &ctrl_vtbl; h->midimap = &mm_vtbl; h->ctxreq = &cr_vtbl;
    tresult r = query(h, (const uint8_t*)iid, obj);
    release(h);
    return r;
}
static const FactoryVtbl factory_vtbl = {f_query, f_ref, f_ref, f_info, f_count, f_class, f_create, f_class2};
const FactoryVtbl* const g_factory_ptr = &factory_vtbl;

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
EXPORT void* GetPluginFactory(void) { return (void*)&g_factory_ptr; }
#ifdef _WIN32
EXPORT int InitDll(void) { return 1; }
EXPORT int ExitDll(void) { return 1; }
#else
EXPORT int ModuleEntry(void*) { return 1; }
EXPORT int ModuleExit(void) { return 1; }
#endif

#include "hosted.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <dlfcn.h>
#endif

namespace mpchost {

namespace {
enum { effOpen = 0, effClose = 1, effGetProgram = 3, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11,
       effMainsChanged = 12, effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effGetEffectName = 45, effGetVendorString = 47 };
// audioMaster opcodes
enum { amAutomate = 0, amVersion = 1, amGetTime = 7, amGetSampleRate = 16, amGetBlockSize = 17, amUpdateDisplay = 42 };

struct GuestTime { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
                   int32_t num, den, smpte, smpteRate, toClock, flags; };

#ifdef _WIN32
constexpr char kSep = ';';
#else
constexpr char kSep = ':';
#endif

std::string exe_dir() {
#ifdef _WIN32
    char b[4096]; const DWORD n = GetModuleFileNameA(nullptr, b, sizeof b); std::string p(b, n);
#else
    char b[4096]; const ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1); std::string p = n > 0 ? std::string(b, (size_t)n) : std::string();
#endif
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
bool exists(const std::string& p) { if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; } return false; }
}  // namespace

struct HostedPlugin::Impl {
    std::unique_ptr<Guest> g;
    bool a64 = false;
    gaddr eff = 0, disp = 0, proc = 0, buf_l = 0, buf_r = 0, outs = 0, ins = 0, evs = 0, ev = 0, time = 0;
    int nev = 0;
    std::vector<std::pair<int, float>> automation;
    bool display_dirty = false;

    int o_disp() const { return a64 ? 8 : 4; }
    int o_setp() const { return a64 ? 24 : 12; }
    int o_getp() const { return a64 ? 32 : 16; }
    int o_nprog() const { return a64 ? 40 : 20; }
    int o_nparams() const { return a64 ? 44 : 24; }
    int o_nout() const { return a64 ? 52 : 32; }
    int o_proc() const { return a64 ? 120 : 80; }
    uint32_t ps() const { return a64 ? 8 : 4; }
    void wrp(gaddr a, uint32_t v) { g->wr32(a, v); if (a64) g->wr32(a + 4, 0); }
    bool dead = false;            // the guest faulted (an unsupported call, a CPU fault, abort): it never runs again, the host process carries on
    std::string fault;
    void kill(const char* m) { dead = true; fault = m; }
    uint32_t callg(gaddr fn, const std::vector<uint32_t>& a, const std::vector<float>& f = {}) { if (dead) return 0; try { return g->call(fn, a, f); } catch (const GuestFault& e) { kill(e.what()); return 0; } }
    float callf(gaddr fn, const std::vector<uint32_t>& a) { if (dead) return 0; try { return g->call_f(fn, a); } catch (const GuestFault& e) { kill(e.what()); return 0; } }
    uint32_t dispatch(int32_t op, int32_t idx, int32_t val, uint32_t ptr, float opt) { return callg(disp, {eff, (uint32_t)op, (uint32_t)idx, (uint32_t)val, ptr}, {opt}); }
    std::string text(int op, int idx) {
        const gaddr b = g->alloc(256); std::memset(g->host(b), 0, 256);
        dispatch(op, idx, 0, b, 0);
        const std::string s(reinterpret_cast<char*>(g->host(b)), strnlen(reinterpret_cast<char*>(g->host(b)), 255)); g->dealloc(b); return s;
    }
};

namespace {
// audioMaster callbacks come without an instance: find it from the guest
std::mutex g_map_mu;
std::unordered_map<Guest*, HostedPlugin::Impl*> g_by_guest;

uint32_t guest_master(Guest& g, uint32_t, int32_t op, int32_t idx, int32_t, uint32_t, float opt) {
    HostedPlugin::Impl* p; { std::lock_guard<std::mutex> l(g_map_mu); auto it = g_by_guest.find(&g); p = it == g_by_guest.end() ? nullptr : it->second; }
    switch (op) {
    case amAutomate: if (p) p->automation.push_back({idx, opt}); return 0;
    case amVersion: return 2400;
    case amGetTime: return p ? p->time : 0;
    case amGetSampleRate: return (uint32_t)HostedPlugin::kRate;
    case amGetBlockSize: return HostedPlugin::kBlock;
    case amUpdateDisplay: if (p) p->display_dirty = true; return 1;
    default: return 0;
    }
}
}  // namespace

HostedPlugin::HostedPlugin() : impl_(new Impl) {}
HostedPlugin::~HostedPlugin() {
    if (impl_->g) { std::lock_guard<std::mutex> l(g_map_mu); g_by_guest.erase(impl_->g.get()); }
}
Guest& HostedPlugin::guest() { return *impl_->g; }
bool HostedPlugin::is64() const { return impl_->a64; }

bool HostedPlugin::open(const std::string& path, const Config& cfg, std::string& error) {
    Impl& p = *impl_;
    // an ELF64 plugin is aarch64 (Gen2), an ELF32 one armhf (Gen1)
    if (FILE* f = std::fopen(path.c_str(), "rb")) { unsigned char id[5] = {}; if (std::fread(id, 1, 5, f) == 5) p.a64 = id[4] == 2; std::fclose(f); }
    else { error = "cannot open " + path; return false; }
    if (!cfg.root.empty()) {
#ifdef _WIN32
        _putenv_s("MPC_HOST_ROOT", cfg.root.c_str());
#else
        ::setenv("MPC_HOST_ROOT", cfg.root.c_str(), 1);
#endif
    }
    p.g.reset(new Guest(p.a64));
    Guest& g = *p.g;
    g.hooks = cfg.hooks;
    const std::string sup = cfg.support_dir.empty() ? exe_dir() : cfg.support_dir;
    auto load = [&](const std::string& f) { if (!g.load(f)) { error = "load " + f + ": " + g.error(); return false; } return true; };
    const std::string helpers = sup + (p.a64 ? "/guest_helpers64.so" : "/guest_helpers.so");
    if (exists(helpers) && !load(helpers)) return false;
    if (!cfg.libs.empty()) { for (auto& l : cfg.libs) if (!load(l)) return false; }
    else if (const char* env = std::getenv("MPC_HOST_LIBS")) {
        std::string l = env;
        for (size_t b = 0; b < l.size();) { size_t e = l.find(kSep, b); if (e == std::string::npos) e = l.size(); if (e > b && !load(l.substr(b, e - b))) return false; b = e + 1; }
    } else {
        const std::string d = sup + "/guestlibs/" + (p.a64 ? "aarch64" : "armv7") + "/";
        for (const char* lib : {"libgcc_s.so.1", "libstdc++.so.6"}) if (exists(d + lib) && !load(d + lib)) return false;
    }
    if (!load(path)) return false;
    const gaddr main = g.symbol("VSTPluginMain");
    if (!main) { error = "no VSTPluginMain"; return false; }
    p.time = g.alloc(sizeof(GuestTime));
    auto* t = g.ptr<GuestTime>(p.time);
    std::memset(t, 0, sizeof *t);
    t->sampleRate = kRate; t->tempo = 120; t->num = 4; t->den = 4; t->flags = (1 << 10) | (1 << 9);
    { std::lock_guard<std::mutex> l(g_map_mu); g_by_guest[&g] = &p; }
    try { p.eff = g.call(main, {g.host_callback(guest_master)}); } catch (const GuestFault& e) { p.kill(e.what()); error = std::string("the plugin faulted while starting: ") + e.what(); return false; }
    if (!p.eff || g.rd32(p.eff) != 0x56737450) { error = "VSTPluginMain returned no AEffect"; return false; }
    p.disp = g.rd32(p.eff + p.o_disp());
    p.proc = g.rd32(p.eff + p.o_proc());
    p.buf_l = g.alloc(kBlock * 4); p.buf_r = g.alloc(kBlock * 4);
    p.outs = g.alloc(16); p.wrp(p.outs, p.buf_l); p.wrp(p.outs + p.ps(), p.buf_r);
    p.ins = g.alloc(16); const gaddr z = g.alloc(kBlock * 4); std::memset(g.host(z), 0, kBlock * 4); p.wrp(p.ins, z); p.wrp(p.ins + p.ps(), z);
    p.evs = g.alloc(16 + 8 * 256); p.ev = g.alloc(32 * 256);
    return true;
}

void HostedPlugin::start() {
    Impl& p = *impl_;
    p.dispatch(effOpen, 0, 0, 0, 0); p.dispatch(effSetSampleRate, 0, 0, 0, (float)kRate); p.dispatch(effSetBlockSize, 0, kBlock, 0, 0); p.dispatch(effMainsChanged, 0, 1, 0, 0);
}

std::string HostedPlugin::name() { return impl_->text(effGetEffectName, 0); }
std::string HostedPlugin::vendor() { return impl_->text(effGetVendorString, 0); }
int HostedPlugin::num_params() const { return (int32_t)impl_->g->rd32(impl_->eff + impl_->o_nparams()); }
int HostedPlugin::num_outputs() const { return (int32_t)impl_->g->rd32(impl_->eff + impl_->o_nout()); }
int HostedPlugin::num_programs() const { return (int32_t)impl_->g->rd32(impl_->eff + impl_->o_nprog()); }
std::string HostedPlugin::param_name(int i) { return impl_->text(effGetParamName, i); }
std::string HostedPlugin::param_label(int i) { return impl_->text(effGetParamLabel, i); }
std::string HostedPlugin::param_display(int i) { return impl_->text(effGetParamDisplay, i); }
void HostedPlugin::set_param(int i, float v) { Impl& p = *impl_; p.callg(p.g->rd32(p.eff + p.o_setp()), {p.eff, (uint32_t)i}, {v}); }
float HostedPlugin::get_param(int i) { Impl& p = *impl_; return p.callf(p.g->rd32(p.eff + p.o_getp()), {p.eff, (uint32_t)i}); }

void HostedPlugin::midi(uint8_t status, uint8_t d1, uint8_t d2, int delta_frames) {
    Impl& p = *impl_;
    if (p.nev >= 256) return;
    const gaddr e = p.ev + 32 * p.nev;
    std::memset(p.g->host(e), 0, 32);
    p.g->wr32(e, 1); p.g->wr32(e + 4, 32); p.g->wr32(e + 8, (uint32_t)delta_frames);   // VstMidiEvent: type 1, byteSize 32, deltaFrames
    p.g->host(e + 24)[0] = status; p.g->host(e + 24)[1] = d1; p.g->host(e + 24)[2] = d2;
    p.wrp(p.evs + (p.a64 ? 16 : 8) + p.ps() * p.nev, e);
    p.nev++;
}
void HostedPlugin::set_transport(double tempo, double ppq, bool playing) {
    Impl& p = *impl_;
    auto* t = p.g->ptr<GuestTime>(p.time);
    t->tempo = tempo; t->ppqPos = ppq; t->flags = (1 << 10) | (1 << 9) | (playing ? 2 : 0);
}
void HostedPlugin::process(float* l, float* r) {
    Impl& p = *impl_; Guest& g = *p.g;
    if (p.nev) { g.wr32(p.evs, p.nev); g.wr32(p.evs + 4, 0); if (p.a64) { g.wr32(p.evs + 8, 0); g.wr32(p.evs + 12, 0); } p.dispatch(effProcessEvents, 0, 0, p.evs, 0); p.nev = 0; }
    p.callg(p.proc, {p.eff, p.ins, p.outs, (uint32_t)kBlock});
    if (p.dead) { std::memset(l, 0, kBlock * 4); std::memset(r, 0, kBlock * 4); return; }   // a dead guest is silent
    std::memcpy(l, g.host(p.buf_l), kBlock * 4);
    std::memcpy(r, g.host(p.buf_r), kBlock * 4);
    g.ptr<GuestTime>(p.time)->samplePos += kBlock;
}

std::vector<uint8_t> HostedPlugin::get_chunk() {
    Impl& p = *impl_; Guest& g = *p.g;
    const gaddr pp = g.alloc(8); g.wr32(pp, 0); g.wr32(pp + 4, 0);
    const int32_t n = (int32_t)p.dispatch(effGetChunk, 0, 0, pp, 0);
    const gaddr data = g.rd32(pp);
    std::vector<uint8_t> c;
    if (n > 0 && data) c.assign(g.host(data), g.host(data) + n);
    g.dealloc(pp);
    return c;
}
bool HostedPlugin::set_chunk(const std::vector<uint8_t>& c) {
    Impl& p = *impl_; Guest& g = *p.g;
    const gaddr b = g.alloc((uint32_t)c.size());
    std::memcpy(g.host(b), c.data(), c.size());
    const bool ok = p.dispatch(effSetChunk, 0, (int32_t)c.size(), b, 0) != 0;
    g.dealloc(b);
    return ok;
}

bool HostedPlugin::dead() const { return impl_->dead; }
std::string HostedPlugin::fault() const { return impl_->fault; }
std::vector<std::pair<int, float>> HostedPlugin::take_automation() { std::vector<std::pair<int, float>> v; v.swap(impl_->automation); return v; }
bool HostedPlugin::take_display_dirty() { const bool d = impl_->display_dirty; impl_->display_dirty = false; return d; }

}  // namespace mpchost

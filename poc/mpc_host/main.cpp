// Prototype driver: renders the same MIDI through an armv7 MPC plugin (emulated) and, optionally, a native build
// of the same port, then reports CPU per block and how close the two outputs are. See docs/MPC_HOST.md.
//   mpc_host <armv7 .so> [native .so] [seconds] [out prefix]
#include "guest.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#endif
#include <memory>
#include <string>
#include <vector>

using namespace mpchost;

namespace {
constexpr int kBlock = 128;
constexpr double kRate = 44100.0;
enum { effOpen = 0, effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effGetEffectName = 45 };

#ifdef _WIN32
constexpr char kPathSep = ';';   // MPC_HOST_LIBS is a list of paths: ';' on Windows (a ':' is part of a drive letter), ':' elsewhere
#else
constexpr char kPathSep = ':';
#endif
#ifndef MPC_HOST_VERSION
#define MPC_HOST_VERSION "dev"
#endif
// the directory the guest helper libraries sit in: next to this program
std::string exe_path() {
#ifdef _WIN32
    char b[4096]; const DWORD n = GetModuleFileNameA(nullptr, b, sizeof b); return std::string(b, n);
#else
    char b[4096]; const ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1); return n > 0 ? std::string(b, (size_t)n) : std::string();
#endif
}

struct Plugin {
    virtual ~Plugin() = default;
    virtual std::string name() = 0;
    virtual int num_params() = 0;
    virtual int num_outputs() = 0;
    virtual void start() = 0;
    virtual void set_param(int i, float v) = 0;
    virtual float get_param(int i) = 0;
    virtual std::vector<uint8_t> get_chunk() = 0;   // empty if the plugin has no state
    virtual bool set_chunk(const std::vector<uint8_t>& c) = 0;
    virtual void midi(const uint8_t m[3]) = 0;   // one event at frame 0 of the next block
    virtual void process(float* l, float* r) = 0;
};

// --- emulated armv7 plugin ---
struct GuestTime { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
                   int32_t num, den, smpte, smpteRate, toClock, flags; };
gaddr g_time = 0;
uint32_t guest_master(Guest&, uint32_t, int32_t op, int32_t, int32_t, uint32_t, float) {
    if (op == 1) return 2400;      // audioMasterVersion
    if (op == 7) return g_time;    // audioMasterGetTime
    return 0;
}

struct Emulated : Plugin {
    Guest g;
    const bool a64;
    // AEffect layout: armhf (4-byte pointers, as in vst2_wrap.c) or aarch64 (8-byte pointers)
    int o_disp() const { return a64 ? 8 : 4; }
    int o_setp() const { return a64 ? 24 : 12; }
    int o_getp() const { return a64 ? 32 : 16; }
    int o_nparams() const { return a64 ? 44 : 24; }
    int o_nout() const { return a64 ? 52 : 32; }
    int o_proc() const { return a64 ? 120 : 80; }
    uint32_t ps() const { return a64 ? 8 : 4; }
    void wrp(gaddr a, uint32_t v) { g.wr32(a, v); if (a64) g.wr32(a + 4, 0); }
    explicit Emulated(bool a64_) : g(a64_), a64(a64_) {}
    gaddr eff = 0, disp = 0, proc = 0, buf_l = 0, buf_r = 0, outs = 0, ins = 0, evs = 0, ev = 0;
    int nev = 0;
    bool open(const char* path) {
        // libc functions that call back into guest code (qsort, bsearch) are guest code too: guest_helpers.so, built next to this program
        {
            const std::string exe = exe_path();
            if (!exe.empty()) { const std::string h = exe.substr(0, exe.find_last_of("/\\")) + (a64 ? "/guest_helpers64.so" : "/guest_helpers.so"); if (FILE* f = std::fopen(h.c_str(), "rb")) { std::fclose(f); if (!g.load(h)) { std::fprintf(stderr, "load %s: %s\n", h.c_str(), g.error().c_str()); return false; } } }
        }
        // MPC_HOST_LIBS=libgcc_s.so.1:libstdc++.so.6 loads armhf libraries first; the plugin's imports then resolve to them.
        // with no MPC_HOST_LIBS, libgcc_s and libstdc++ for this architecture are taken from guestlibs/<armv7|aarch64>/ next to this program, if they are there
        if (!std::getenv("MPC_HOST_LIBS")) {
            const std::string exe = exe_path();
            if (!exe.empty()) {
                const std::string d = exe.substr(0, exe.find_last_of("/\\")) + "/guestlibs/" + (a64 ? "aarch64" : "armv7") + "/";
                for (const char* lib : {"libgcc_s.so.1", "libstdc++.so.6"})
                    if (FILE* f = std::fopen((d + lib).c_str(), "rb")) { std::fclose(f); if (!g.load(d + lib)) { std::fprintf(stderr, "load %s%s: %s\n", d.c_str(), lib, g.error().c_str()); return false; } }
            }
        }
        if (const char* libs = std::getenv("MPC_HOST_LIBS")) {
            std::string l = libs;
            for (size_t b = 0; b < l.size();) {
                size_t e = l.find(kPathSep, b); if (e == std::string::npos) e = l.size();
                if (e > b && !g.load(l.substr(b, e - b))) { std::fprintf(stderr, "load %s: %s\n", l.substr(b, e - b).c_str(), g.error().c_str()); return false; }
                b = e + 1;
            }
        }
        if (!g.load(path)) { std::fprintf(stderr, "load: %s\n", g.error().c_str()); return false; }
        const gaddr main = g.symbol("VSTPluginMain");
        if (!main) { std::fprintf(stderr, "no VSTPluginMain\n"); return false; }
        g_time = g.alloc(sizeof(GuestTime));
        auto* t = g.ptr<GuestTime>(g_time);
        std::memset(t, 0, sizeof *t);
        t->sampleRate = kRate; t->tempo = 120; t->num = 4; t->den = 4; t->flags = (1 << 10) | (1 << 9);
        eff = g.call(main, {g.host_callback(guest_master)});
        if (!eff || g.rd32(eff) != 0x56737450) { std::fprintf(stderr, "VSTPluginMain returned no AEffect\n"); return false; }
        disp = g.rd32(eff + o_disp());
        proc = g.rd32(eff + o_proc());       // processReplacing (see vst2_wrap.c struct AEffect)
        buf_l = g.alloc(kBlock * 4); buf_r = g.alloc(kBlock * 4);
        outs = g.alloc(16); wrp(outs, buf_l); wrp(outs + ps(), buf_r);
        ins = g.alloc(16); const gaddr z = g.alloc(kBlock * 4); std::memset(g.host(z), 0, kBlock * 4); wrp(ins, z); wrp(ins + ps(), z);
        evs = g.alloc(16 + 8 * 64); ev = g.alloc(32 * 64);
        return true;
    }
    uint32_t dispatch(int32_t op, int32_t idx, int32_t val, uint32_t ptr, float opt) { return g.call(disp, {eff, (uint32_t)op, (uint32_t)idx, (uint32_t)val, ptr}, {opt}); }
    std::string name() override { const gaddr b = g.alloc(256); g.host(b)[0] = 0; dispatch(effGetEffectName, 0, 0, b, 0); std::string s = (char*)g.host(b); g.dealloc(b); return s; }
    int num_params() override { return (int32_t)g.rd32(eff + o_nparams()); }
    int num_outputs() override { return (int32_t)g.rd32(eff + o_nout()); }
    void start() override { dispatch(effOpen, 0, 0, 0, 0); dispatch(effSetSampleRate, 0, 0, 0, (float)kRate); dispatch(effSetBlockSize, 0, kBlock, 0, 0); dispatch(effMainsChanged, 0, 1, 0, 0); }
    void set_param(int i, float v) override { g.call(g.rd32(eff + o_setp()), {eff, (uint32_t)i}, {v}); }
    float get_param(int i) override { return g.call_f(g.rd32(eff + o_getp()), {eff, (uint32_t)i}); }
    std::vector<uint8_t> get_chunk() override {
        const gaddr pp = g.alloc(8); g.wr32(pp, 0); g.wr32(pp + 4, 0);
        const int32_t n = (int32_t)dispatch(effGetChunk, 0, 0, pp, 0);
        const gaddr data = g.rd32(pp);
        std::vector<uint8_t> c;
        if (n > 0 && data) c.assign(g.host(data), g.host(data) + n);
        g.dealloc(pp);
        return c;
    }
    bool set_chunk(const std::vector<uint8_t>& c) override {
        const gaddr b = g.alloc((uint32_t)c.size());
        std::memcpy(g.host(b), c.data(), c.size());
        const bool ok = dispatch(effSetChunk, 0, (int32_t)c.size(), b, 0) != 0;
        g.dealloc(b);
        return ok;
    }
    void midi(const uint8_t m[3]) override {
        const gaddr e = ev + 32 * nev;
        std::memset(g.host(e), 0, 32);
        g.wr32(e, 1); g.wr32(e + 4, 32);
        std::memcpy(g.host(e + 24), m, 3);
        wrp(evs + (a64 ? 16 : 8) + ps() * nev, e);
        nev++;
    }
    void process(float* l, float* r) override {
        if (nev) { g.wr32(evs, nev); g.wr32(evs + 4, 0); if (a64) { g.wr32(evs + 8, 0); g.wr32(evs + 12, 0); } dispatch(effProcessEvents, 0, 0, evs, 0); nev = 0; }
        g.call(proc, {eff, ins, outs, (uint32_t)kBlock});
        std::memcpy(l, g.host(buf_l), kBlock * 4);
        std::memcpy(r, g.host(buf_r), kBlock * 4);
        g.ptr<GuestTime>(g_time)->samplePos += kBlock;
    }
};

#ifndef _WIN32
// --- native build of the same port (host AEffect layout) ---
struct AEffect;
typedef intptr_t (*Master)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
    void (*process)(AEffect*, float**, float**, int32_t);
    void (*setParameter)(AEffect*, int32_t, float);
    float (*getParameter)(AEffect*, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect*, float**, float**, int32_t);
};
struct NativeEvent { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; uint8_t midi[4]; char d[4]; };
struct NativeEvents { int32_t n; intptr_t reserved; void* ev[64]; };
GuestTime n_time;
intptr_t native_master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;
    if (op == 7) return (intptr_t)&n_time;
    return 0;
}
struct Native : Plugin {
    AEffect* e = nullptr;
    NativeEvent ev[64]; NativeEvents evs; int nev = 0;
    float zero[kBlock] = {};
    bool open(const char* path) {
        void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return false; }
        auto main = (AEffect * (*)(Master)) dlsym(h, "VSTPluginMain");
        std::memset(&n_time, 0, sizeof n_time);
        n_time.sampleRate = kRate; n_time.tempo = 120; n_time.num = 4; n_time.den = 4; n_time.flags = (1 << 10) | (1 << 9);
        return main && (e = main(native_master));
    }
    std::string name() override { char b[256] = {}; e->dispatcher(e, effGetEffectName, 0, 0, b, 0); return b; }
    int num_params() override { return e->numParams; }
    int num_outputs() override { return e->numOutputs; }
    void start() override { e->dispatcher(e, effOpen, 0, 0, nullptr, 0); e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)kRate); e->dispatcher(e, effSetBlockSize, 0, kBlock, nullptr, 0); e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0); }
    void set_param(int i, float v) override { e->setParameter(e, i, v); }
    float get_param(int i) override { return e->getParameter(e, i); }
    std::vector<uint8_t> get_chunk() override {
        void* d = nullptr;
        const intptr_t n = e->dispatcher(e, effGetChunk, 0, 0, &d, 0);
        return n > 0 && d ? std::vector<uint8_t>((uint8_t*)d, (uint8_t*)d + n) : std::vector<uint8_t>();
    }
    bool set_chunk(const std::vector<uint8_t>& c) override { return e->dispatcher(e, effSetChunk, 0, (int32_t)c.size(), (void*)c.data(), 0) != 0; }
    void midi(const uint8_t m[3]) override { std::memset(&ev[nev], 0, sizeof ev[nev]); ev[nev].type = 1; ev[nev].byteSize = 32; std::memcpy(ev[nev].midi, m, 3); evs.ev[nev] = &ev[nev]; nev++; }
    void process(float* l, float* r) override {
        if (nev) { evs.n = nev; evs.reserved = 0; e->dispatcher(e, effProcessEvents, 0, 0, &evs, 0); nev = 0; }
        float* in[2] = {zero, zero}; float* out[2] = {l, r};
        e->processReplacing(e, in, out, kBlock);
        n_time.samplePos += kBlock;
    }
};

#endif  // _WIN32

// A fixed 16th-note acid line: notes, accents, slides via overlapping notes.
void events_for_block(int b, Plugin& p) {
    static const int notes[16] = {36, 36, 48, 36, 39, 36, 43, 41, 36, 48, 36, 46, 36, 39, 51, 36};
    const int step_blocks = (int)(kRate * 0.125 / kBlock);   // 16ths at 120 BPM
    if (b % step_blocks) return;
    const int step = b / step_blocks;
    const int prev = notes[(step + 15) % 16], cur = notes[step % 16];
    if (step) { const uint8_t off[3] = {0x80, (uint8_t)prev, 0}; p.midi(off); }
    const uint8_t on[3] = {0x90, (uint8_t)cur, (uint8_t)((step % 4 == 0) ? 127 : 90)};
    p.midi(on);
}

struct Run { std::vector<float> l, r; std::vector<double> us; };
Run render(Plugin& p, int blocks) {
    Run out; out.l.resize((size_t)blocks * kBlock); out.r.resize(out.l.size()); out.us.reserve(blocks);
    p.start();
    for (int b = 0; b < blocks; b++) {
        events_for_block(b, p);
        const auto t0 = std::chrono::steady_clock::now();
        p.process(&out.l[(size_t)b * kBlock], &out.r[(size_t)b * kBlock]);
        out.us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    return out;
}

void write_wav(const std::string& path, const Run& r) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)r.l.size(), data = n * 4, rate = (uint32_t)kRate, br = rate * 4;
    const uint16_t ch = 2, bits = 16, align = 4, fmt = 1;
    const uint32_t riff = 36 + data, fsz = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fsz, 4, 1, f);
    std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f);
    std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (uint32_t k = 0; k < n; k++)
        for (float s : {r.l[k], r.r[k]}) { const int16_t v = (int16_t)std::lrint(std::fmax(-1.f, std::fmin(1.f, s)) * 32767); std::fwrite(&v, 2, 1, f); }
    std::fclose(f);
}

void report(const char* what, const Run& r) {
    std::vector<double> us(r.us.begin() + std::min<size_t>(r.us.size(), 50), r.us.end());   // skip warm-up (JIT compiles)
    std::sort(us.begin(), us.end());
    double sum = 0, peak = 0; for (float s : r.l) { sum += (double)s * s; peak = std::fmax(peak, std::fabs(s)); }
    const double budget = kBlock / kRate * 1e6;
    const double med = us[us.size() / 2], p99 = us[us.size() * 99 / 100];
    std::printf("%-9s median %7.1f us  p99 %7.1f us  max %7.1f us  (%.1f%% of a %.0f us block, median)  rms %.4f peak %.3f\n",
                what, med, p99, us.back(), 100 * med / budget, budget, std::sqrt(sum / r.l.size()), peak);
}
// Parameters and state: take up to 8 parameters that read back what was set (readouts do not), set them to 0.25, save the chunk,
// set them to 0.75, restore the chunk, and expect each parameter to read what it read before the save.
std::string g_state_note;   // a one-word summary of the last state_check, for --check
bool state_check(const char* what, Plugin& p) {
    std::vector<int> idx; std::vector<float> saved;
    for (int i = 0; i < p.num_params() && idx.size() < 8; i++) {
        p.set_param(i, 0.25f); const float v = p.get_param(i);
        p.set_param(i, 0.75f); const float w = p.get_param(i);
        if (std::getenv("MPC_HOST_DUMP")) std::printf("  param %d: set 0.25 reads %.3f, set 0.75 reads %.3f\n", i, v, w);
        if (std::fabs(v - 0.25f) < 0.15f && std::fabs(w - 0.75f) < 0.15f && w > v) idx.push_back(i);   // quantised parameters round
    }
    if (idx.empty()) { g_state_note = "no settable parameter"; std::printf("%-9s state: no settable parameter among %d\n", what, p.num_params()); return true; }
    for (int i : idx) { p.set_param(i, 0.25f); saved.push_back(p.get_param(i)); }
    const auto chunk = p.get_chunk();
    for (int i : idx) p.set_param(i, 0.75f);
    if (chunk.empty()) { g_state_note = "no chunk"; std::printf("%-9s state: %zu settable parameters, no chunk (ok)\n", what, idx.size()); return true; }
    const bool restored = p.set_chunk(chunk);
    { float l[kBlock], r[kBlock]; p.process(l, r); p.process(l, r); }   // some ports apply a restored state on the next audio block
    std::string bad; int good = 0;
    for (size_t k = 0; k < idx.size(); k++) {
        const float c = p.get_param(idx[k]);
        if (std::fabs(c - saved[k]) < 0.005f) good++; else { char t[64]; std::snprintf(t, sizeof t, " %d:%.3f->%.3f", idx[k], saved[k], c); bad += t; }
    }
    const bool ok = restored && bad.empty();
    g_state_note = ok ? "round-trips" : good > 0 ? "partial" : "not restored";
    if (!ok && std::getenv("MPC_HOST_DUMP")) { const auto after = p.get_chunk(); std::printf("chunk before: %.*s\nchunk after:  %.*s\n", (int)std::min<size_t>(chunk.size(), 300), (const char*)chunk.data(), (int)std::min<size_t>(after.size(), 300), (const char*)after.data()); }
    std::printf("%-9s state: %d/%zu parameters restored from a %zu-byte chunk (%s)%s\n", what, good, idx.size(), chunk.size(), ok ? "ok" : "FAILED", bad.c_str());
    return ok;
}
}  // namespace

// --- --check: decide whether the host should use a plugin at all -------------------------------------------------------------------
// The probe runs in a child process (a guest abort or a hang must not take the caller down). It loads the plugin, runs two seconds of the test line at
// real-time block size with unsupported imports stubbed (so the ones it actually calls are recorded), and checks the state round trip.
// Verdicts: "blocked" (do not use: fails to load or start, crashes, hangs, calls unsupported functions, or is too slow for real time),
// "degraded" (usable with a warning: heavy CPU use, or state that does not fully round-trip), "ok".
constexpr double kBlockedPct = 80, kDegradedPct = 50, kP99BlockedPct = 100, kP99DegradedPct = 80;

std::string jesc(const std::string& s) {
    std::string o; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((unsigned char)c < 32) o += ' '; else o += c; } return o;
}
std::string jlist(const std::vector<std::string>& v) { std::string o = "["; for (size_t k = 0; k < v.size(); k++) { if (k) o += ","; o += "\"" + jesc(v[k]) + "\""; } return o + "]"; }

int probe(const char* path, int out_fd, const char* prefix = "") {
#ifdef _WIN32
    _putenv_s("MPC_HOST_LAX", "1");
#else
    ::setenv("MPC_HOST_LAX", "1", 1);
#endif   // before the first unsupported call: record them instead of stopping
    bool a64 = false;
    if (FILE* f = std::fopen(path, "rb")) { unsigned char id[5] = {}; if (std::fread(id, 1, 5, f) == 5) a64 = id[4] == 2; std::fclose(f); }
    std::vector<std::string> reasons, warnings;
    std::string verdict = "ok", name;
    double med_pct = 0, p99_pct = 0, rms = 0;
    std::string state = "n/a";
    auto emu = std::make_unique<Emulated>(a64);
    if (!emu->open(path)) { verdict = "blocked"; reasons.push_back("could not load or start the plugin"); }
    else {
        name = emu->name();
        const Run a = render(*emu, (int)(2 * kRate / kBlock));
        std::vector<double> us(a.us.begin() + std::min<size_t>(a.us.size(), 50), a.us.end());   // skip the JIT warm-up
        std::sort(us.begin(), us.end());
        const double budget = kBlock / kRate * 1e6;
        if (!us.empty()) { med_pct = 100 * us[us.size() / 2] / budget; p99_pct = 100 * us[us.size() * 99 / 100] / budget; }
        double sum = 0; for (float s : a.l) sum += (double)s * s; rms = std::sqrt(sum / std::max<size_t>(1, a.l.size()));
        const bool state_ok = state_check("emulated", *emu);
        state = g_state_note.empty() ? "n/a" : g_state_note;
        if (!emu->g.stubs_called().empty()) { verdict = "blocked"; reasons.push_back("calls functions the host does not provide: " + [&] { std::string s; for (auto& n : emu->g.stubs_called()) s += (s.empty() ? "" : ", ") + n; return s; }()); }
        if (med_pct >= kBlockedPct || p99_pct >= kP99BlockedPct) { verdict = "blocked"; char b[160]; std::snprintf(b, sizeof b, "too slow for real time here: median %.0f%% and p99 %.0f%% of a block", med_pct, p99_pct); reasons.push_back(b); }
        else if (med_pct >= kDegradedPct || p99_pct >= kP99DegradedPct) { if (verdict == "ok") verdict = "degraded"; char b[160]; std::snprintf(b, sizeof b, "heavy: median %.0f%% and p99 %.0f%% of a block", med_pct, p99_pct); warnings.push_back(b); }
        if (!state_ok) { if (verdict == "ok") verdict = "degraded"; warnings.push_back("state does not fully round-trip (" + state + ")"); }
    }
    char num[200]; std::snprintf(num, sizeof num, "\"median_pct\":%.1f,\"p99_pct\":%.1f,\"rms\":%.4f", med_pct, p99_pct, rms);
    const std::string json = "{\"plugin\":\"" + jesc(path) + "\",\"name\":\"" + jesc(name) + "\",\"arch\":\"" + (a64 ? "aarch64" : "armv7") + "\",\"verdict\":\"" + verdict + "\",\"reasons\":" +
                             jlist(reasons) + ",\"warnings\":" + jlist(warnings) + "," + num + ",\"state\":\"" + jesc(state) + "\"}\n";
    const std::string line = prefix + json;
#ifdef _WIN32
    (void)out_fd; std::fwrite(line.data(), 1, line.size(), stdout); std::fflush(stdout);
#else
    (void)!::write(out_fd, line.data(), line.size());
#endif
    return 0;
}

#ifdef _WIN32
// Windows: run this program again as `--probe <plugin>` with stdout and stderr on one pipe; the probe's JSON line starts with MPCHOST_JSON.
int check_main(const char* path, int timeout_s) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 4;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::string cmd = "\"" + exe_path() + "\" --probe \"" + path + "\"";
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return 4; }
    CloseHandle(wr);
    std::string out; bool timed_out = false;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) { char buf[4096]; DWORD n = 0; if (!ReadFile(rd, buf, sizeof buf, &n, nullptr) || !n) break; out.append(buf, n); }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {   // exited: take what is left
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) { char buf[4096]; DWORD n = 0; if (!ReadFile(rd, buf, sizeof buf, &n, nullptr) || !n) break; out.append(buf, n); }
            break;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout_s) { timed_out = true; TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); break; }
    }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    std::string json; const size_t mk = out.rfind("MPCHOST_JSON ");
    if (mk != std::string::npos) json = out.substr(mk + 13, out.find('\n', mk) - mk - 13) + "\n";
    if (json.empty()) {
        const std::string why = timed_out ? "hung: no result within " + std::to_string(timeout_s) + " s" : "crashed (exit code " + std::to_string(code) + ")";
        std::string hint; const size_t pos = out.rfind("guest:");
        if (pos != std::string::npos) hint = out.substr(pos, out.find('\n', pos) - pos);
        json = "{\"plugin\":\"" + jesc(path) + "\",\"verdict\":\"blocked\",\"reasons\":" + jlist({why + (hint.empty() ? "" : ": " + hint)}) + ",\"warnings\":[]}\n";
    }
    std::fputs(json.c_str(), stdout);
    return json.find("\"verdict\":\"blocked\"") != std::string::npos ? 4 : json.find("\"verdict\":\"degraded\"") != std::string::npos ? 3 : 0;
}
#else
// Runs probe() in a child with a time limit. Exit code: 0 ok, 3 degraded, 4 blocked. Prints one JSON line.
int check_main(const char* path, int timeout_s) {
    int jp[2], ep[2];
    if (pipe(jp) || pipe(ep)) { std::perror("pipe"); return 4; }
    const pid_t pid = fork();
    if (pid == 0) {
        ::close(jp[0]); ::close(ep[0]);
        ::dup2(ep[1], 2); ::dup2(ep[1], 1);   // everything the guest and the probe print goes to the diagnostics pipe
        ::_exit(probe(path, jp[1]));
    }
    ::close(jp[1]); ::close(ep[1]);
    std::string json, diag;
    const auto t0 = std::chrono::steady_clock::now();
    bool timed_out = false;
    pollfd fds[2] = {{jp[0], POLLIN, 0}, {ep[0], POLLIN, 0}};
    int open_fds = 2;
    while (open_fds > 0) {
        const double left = timeout_s - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (left <= 0) { timed_out = true; ::kill(pid, SIGKILL); break; }
        if (::poll(fds, 2, (int)(left * 1000)) <= 0) continue;
        for (int k = 0; k < 2; k++) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP))) continue;
            char buf[4096]; const ssize_t n = ::read(fds[k].fd, buf, sizeof buf);
            if (n <= 0) { fds[k].fd = -1; open_fds--; } else (k == 0 ? json : diag).append(buf, (size_t)n);
        }
    }
    int status = 0; ::waitpid(pid, &status, 0);
    if (json.empty()) {   // the child died or hung before it could report
        std::string why = timed_out ? "hung: no result within " + std::to_string(timeout_s) + " s"
                          : WIFSIGNALED(status) ? "crashed (signal " + std::to_string(WTERMSIG(status)) + ")" : "exited without a result";
        // the guest's last words, e.g. "guest: called X (not supported)"
        std::string hint; size_t pos = diag.rfind("guest:");
        if (pos != std::string::npos) hint = diag.substr(pos, diag.find('\n', pos) - pos);
        json = "{\"plugin\":\"" + jesc(path) + "\",\"verdict\":\"blocked\",\"reasons\":" + jlist({why + (hint.empty() ? "" : ": " + hint)}) + ",\"warnings\":[]}\n";
    }
    std::fputs(json.c_str(), stdout);
    return json.find("\"verdict\":\"blocked\"") != std::string::npos ? 4 : json.find("\"verdict\":\"degraded\"") != std::string::npos ? 3 : 0;
}
#endif

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin .so> [native .so] [seconds] [out prefix]\n       %s --check <plugin .so> [timeout seconds]   (prints a JSON verdict; exit 0 ok, 3 degraded, 4 blocked)\n", argv[0], argv[0]); return 2; }
    if (!std::strcmp(argv[1], "--version")) { std::printf("mpc_host %s\n", MPC_HOST_VERSION); return 0; }
    if (!std::strcmp(argv[1], "--probe") && argc > 2) return probe(argv[2], 1, "MPCHOST_JSON ");
    if (!std::strcmp(argv[1], "--check")) { if (argc < 3) return 2; return check_main(argv[2], argc > 3 ? std::atoi(argv[3]) : 180); }
    // MPC_HOST_ENFORCE=1: check the plugin first and refuse a blocked one (MPC_HOST_FORCE=1 overrides). A host that scans for plugins does the same with --check.
    if (std::getenv("MPC_HOST_ENFORCE") && !std::getenv("MPC_HOST_FORCE")) {
        const int rc = check_main(argv[1], 180);
        if (rc == 4) { std::fprintf(stderr, "mpc_host: refusing %s (blocked, see above); MPC_HOST_FORCE=1 runs it anyway\n", argv[1]); return 4; }
    }
    const double secs = argc > 3 ? std::atof(argv[3]) : 10;
    const std::string prefix = argc > 4 ? argv[4] : "mpc_host";
    const int blocks = (int)(secs * kRate / kBlock);

    // an ELF64 plugin is aarch64 (Gen2), an ELF32 one armhf (Gen1)
    bool a64 = false;
    if (FILE* f = std::fopen(argv[1], "rb")) { unsigned char id[5] = {}; if (std::fread(id, 1, 5, f) == 5) a64 = id[4] == 2; std::fclose(f); }
    auto emu = std::make_unique<Emulated>(a64);
    if (!emu->open(argv[1])) return 1;
    std::printf("emulated: \"%s\", %d params, %d outputs\n", emu->name().c_str(), emu->num_params(), emu->num_outputs());
    const Run a = render(*emu, blocks);
    report("emulated", a);
    write_wav(prefix + "_emulated.wav", a);
    bool ok = state_check("emulated", *emu);

#ifndef _WIN32
    if (argc > 2 && std::strcmp(argv[2], "-")) {
        auto nat = std::make_unique<Native>();
        if (!nat->open(argv[2])) return 1;
        const Run b = render(*nat, blocks);
        report("native", b);
        write_wav(prefix + "_native.wav", b);
        ok &= state_check("native", *nat);
        double diff = 0, ref = 0, maxd = 0;
        for (size_t k = 0; k < a.l.size(); k++) { const double d = a.l[k] - b.l[k]; diff += d * d; ref += (double)b.l[k] * b.l[k]; maxd = std::fmax(maxd, std::fabs(d)); }
        std::printf("difference: %.1f dB below the native signal, max %.5f\n", 10 * std::log10(ref / (diff + 1e-30)), maxd);
    }
#endif
    return ok ? 0 : 1;
}

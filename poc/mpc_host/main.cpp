// Prototype driver: renders the same MIDI through an armv7 MPC plugin (emulated) and, optionally, a native build
// of the same port, then reports CPU per block and how close the two outputs are. See docs/MPC_HOST.md.
//   mpc_host <armv7 .so> [native .so] [seconds] [out prefix]
#include "guest.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <vector>

using namespace mpchost;

namespace {
constexpr int kBlock = 128;
constexpr double kRate = 44100.0;
enum { effOpen = 0, effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effGetEffectName = 45 };

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
    gaddr eff = 0, disp = 0, proc = 0, buf_l = 0, buf_r = 0, outs = 0, ins = 0, evs = 0, ev = 0;
    int nev = 0;
    bool open(const char* path) {
        // MPC_HOST_LIBS=libgcc_s.so.1:libstdc++.so.6 loads armhf libraries first; the plugin's imports then resolve to them.
        if (const char* libs = std::getenv("MPC_HOST_LIBS")) {
            std::string l = libs;
            for (size_t b = 0; b < l.size();) {
                size_t e = l.find(':', b); if (e == std::string::npos) e = l.size();
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
        disp = g.rd32(eff + 4);
        proc = g.rd32(eff + 80);       // processReplacing (32-bit layout: see vst2_wrap.c struct AEffect)
        buf_l = g.alloc(kBlock * 4); buf_r = g.alloc(kBlock * 4);
        outs = g.alloc(8); g.wr32(outs, buf_l); g.wr32(outs + 4, buf_r);
        ins = g.alloc(8); const gaddr z = g.alloc(kBlock * 4); std::memset(g.host(z), 0, kBlock * 4); g.wr32(ins, z); g.wr32(ins + 4, z);
        evs = g.alloc(8 + 4 * 64); ev = g.alloc(32 * 64);
        return true;
    }
    uint32_t dispatch(int32_t op, int32_t idx, int32_t val, uint32_t ptr, float opt) { return g.call(disp, {eff, (uint32_t)op, (uint32_t)idx, (uint32_t)val, ptr}, {opt}); }
    std::string name() override { const gaddr b = g.alloc(256); g.host(b)[0] = 0; dispatch(effGetEffectName, 0, 0, b, 0); std::string s = (char*)g.host(b); g.dealloc(b); return s; }
    int num_params() override { return (int32_t)g.rd32(eff + 24); }
    int num_outputs() override { return (int32_t)g.rd32(eff + 32); }
    void start() override { dispatch(effOpen, 0, 0, 0, 0); dispatch(effSetSampleRate, 0, 0, 0, (float)kRate); dispatch(effSetBlockSize, 0, kBlock, 0, 0); dispatch(effMainsChanged, 0, 1, 0, 0); }
    void set_param(int i, float v) override { g.call(g.rd32(eff + 12), {eff, (uint32_t)i}, {v}); }   // setParameter at byte 12
    float get_param(int i) override { return g.call_f(g.rd32(eff + 16), {eff, (uint32_t)i}); }        // getParameter at byte 16
    std::vector<uint8_t> get_chunk() override {
        const gaddr pp = g.alloc(4); g.wr32(pp, 0);
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
        g.wr32(evs + 8 + 4 * nev, e);
        nev++;
    }
    void process(float* l, float* r) override {
        if (nev) { g.wr32(evs, nev); g.wr32(evs + 4, 0); dispatch(effProcessEvents, 0, 0, evs, 0); nev = 0; }
        g.call(proc, {eff, ins, outs, (uint32_t)kBlock});
        std::memcpy(l, g.host(buf_l), kBlock * 4);
        std::memcpy(r, g.host(buf_r), kBlock * 4);
        g.ptr<GuestTime>(g_time)->samplePos += kBlock;
    }
};

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
bool state_check(const char* what, Plugin& p) {
    std::vector<int> idx; std::vector<float> saved;
    for (int i = 0; i < p.num_params() && idx.size() < 8; i++) {
        p.set_param(i, 0.25f); const float v = p.get_param(i);
        p.set_param(i, 0.75f); const float w = p.get_param(i);
        if (std::fabs(v - 0.25f) < 0.15f && std::fabs(w - 0.75f) < 0.15f && w > v) idx.push_back(i);   // quantised parameters round
    }
    if (idx.empty()) { std::printf("%-9s state: no settable parameter among %d\n", what, p.num_params()); return true; }
    for (int i : idx) { p.set_param(i, 0.25f); saved.push_back(p.get_param(i)); }
    const auto chunk = p.get_chunk();
    for (int i : idx) p.set_param(i, 0.75f);
    if (chunk.empty()) { std::printf("%-9s state: %zu settable parameters, no chunk (ok)\n", what, idx.size()); return true; }
    const bool restored = p.set_chunk(chunk);
    std::string bad; int good = 0;
    for (size_t k = 0; k < idx.size(); k++) {
        const float c = p.get_param(idx[k]);
        if (std::fabs(c - saved[k]) < 0.005f) good++; else { char t[64]; std::snprintf(t, sizeof t, " %d:%.3f->%.3f", idx[k], saved[k], c); bad += t; }
    }
    const bool ok = restored && bad.empty();
    if (!ok && std::getenv("MPC_HOST_DUMP")) { const auto after = p.get_chunk(); std::printf("chunk before: %.*s\nchunk after:  %.*s\n", (int)std::min<size_t>(chunk.size(), 300), (const char*)chunk.data(), (int)std::min<size_t>(after.size(), 300), (const char*)after.data()); }
    std::printf("%-9s state: %d/%zu parameters restored from a %zu-byte chunk (%s)%s\n", what, good, idx.size(), chunk.size(), ok ? "ok" : "FAILED", bad.c_str());
    return ok;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <armv7 .so> [native .so] [seconds] [out prefix]\n", argv[0]); return 2; }
    const double secs = argc > 3 ? std::atof(argv[3]) : 10;
    const std::string prefix = argc > 4 ? argv[4] : "mpc_host";
    const int blocks = (int)(secs * kRate / kBlock);

    auto emu = std::make_unique<Emulated>();
    if (!emu->open(argv[1])) return 1;
    std::printf("emulated: \"%s\", %d params, %d outputs\n", emu->name().c_str(), emu->num_params(), emu->num_outputs());
    const Run a = render(*emu, blocks);
    report("emulated", a);
    write_wav(prefix + "_emulated.wav", a);
    bool ok = state_check("emulated", *emu);

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
    return ok ? 0 : 1;
}

// Prototype guest: 4 GB guest space, a small ELF32 loader for armhf shared objects, dynarmic A32, and thunks that
// route the plugin's libc/libm/libstdc++ imports to host code. See docs/MPC_HOST.md.
#include "guest.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <glob.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <elf.h>
#include <fstream>
#include <functional>
#include <map>
#include <sys/mman.h>
#include <unordered_map>
#include <cwctype>

namespace mpchost {

namespace {
constexpr uint64_t kSpace = 1ull << 32;
constexpr gaddr kThunks = 0x00001000;     // svc #n; bx lr  per import (8 bytes each), up to 0x7000
constexpr gaddr kData = 0x00008000;       // misc guest data (ctype table, stack guard, nothrow)
constexpr gaddr kTp = 0x000C0000;         // thread pointer: TLS blocks follow it (ARM variant 1: 8-byte TCB), up to kLoad
constexpr gaddr kLoad = 0x00100000;       // load bias of the first module; later modules follow it, 64 KB aligned
constexpr gaddr kHeap = 0x10000000;
constexpr gaddr kHeapEnd = 0xE0000000;
constexpr gaddr kStackTop = 0xF0000000;   // 8 MB stack below this
constexpr uint32_t kRetSvc = 0xFFFFFF;    // svc number of the "return to host" stub
}  // namespace

struct Impl;
using Thunk = std::function<void(Guest::Impl&)>;

struct Guest::Impl final : Dynarmic::A32::UserCallbacks {
    Guest& g;
    uint8_t* m;
    Dynarmic::ExclusiveMonitor monitor{1};   // LDREX/STREX (one guest CPU)
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::vector<Thunk> thunks;               // index = svc number
    std::vector<std::string> thunk_names;
    std::unordered_map<std::string, gaddr> dynsyms;
    gaddr data_next = kData;
    gaddr load_next = kLoad;
    std::unordered_map<std::string, gaddr> thunk_by_name, data_by_name;
    struct TlsMod { gaddr block; uint32_t off; };   // block address, offset from the thread pointer's TLS start
    std::vector<TlsMod> tls;                        // module id - 1
    uint32_t tls_used = 0;
    std::vector<uint32_t> keys;   // pthread_key values
    struct Mod { gaddr base, end, exidx; uint32_t exidx_size; };   // for __gnu_Unwind_Find_exidx (ARM EHABI unwinding)
    std::vector<Mod> mods;
    gaddr errno_p = 0, clocale = 0;
    // --- files: guest paths are mapped into MPC_HOST_ROOT (so /sdcard/x is $MPC_HOST_ROOT/sdcard/x); /plugin/ is the plugin's own directory ---
    std::unordered_map<gaddr, FILE*> files;
    std::unordered_map<gaddr, DIR*> dirs;
    std::string plugin_dir;
    std::vector<std::string> mod_paths;   // parallel to mods
    bool hostpath(const char* gp, std::string& out) {
        std::string p = gp ? gp : "";
        if (p.empty()) return false;
        if (p[0] != '/') p = "/" + p;
        if (p.find("/../") != std::string::npos || (p.size() > 3 && p.compare(p.size() - 3, 3, "/..") == 0)) return false;
        if (p == "/plugin" || p.rfind("/plugin/", 0) == 0) { out = plugin_dir + p.substr(7); return true; }
        if (const char* r = std::getenv("MPC_HOST_ROOT")) { out = std::string(r) + p; return true; }
        return false;
    }
    std::string guestpath(const std::string& hp) {
        if (!plugin_dir.empty() && hp.rfind(plugin_dir, 0) == 0) return "/plugin" + hp.substr(plugin_dir.size());
        if (const char* r = std::getenv("MPC_HOST_ROOT")) { const std::string rr = r; if (hp.rfind(rr, 0) == 0) return hp.substr(rr.size()); }
        return hp;
    }
    void set_errno(int e) { if (!errno_p) errno_p = data(4, 4); wr<int32_t>(errno_p, e); }
    uint32_t fail(int e) { set_errno(e); return 0xFFFFFFFFu; }
    FILE* file_of(gaddr f) { auto it = files.find(f); return it == files.end() ? nullptr : it->second; }
    std::unordered_map<std::string, gaddr> cstrs;
    gaddr c_locale() { if (!clocale) clocale = data(64, 8); return clocale; }
    gaddr cstr(const char* t) { auto& a = cstrs[t]; if (!a) { a = data((uint32_t)std::strlen(t) + 1, 1); std::strcpy((char*)m + a, t); } return a; }
    gaddr heap_next = kHeap;
    std::map<uint32_t, std::vector<gaddr>> free_lists;   // by rounded size
    std::unordered_map<gaddr, uint32_t> sizes;
    int depth = 0;

    explicit Impl(Guest& g_) : g(g_), m(g_.mem_) {}

    // --- dynarmic callbacks (fastmem covers almost everything; these are the slow path) ---
    template <class T> T rd(uint32_t a) { T v; std::memcpy(&v, m + a, sizeof v); return v; }
    template <class T> void wr(uint32_t a, T v) { std::memcpy(m + a, &v, sizeof v); }
    uint8_t MemoryRead8(uint32_t a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(uint32_t a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(uint32_t a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(uint32_t a) override { return rd<uint64_t>(a); }
    void MemoryWrite8(uint32_t a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(uint32_t a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(uint32_t a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(uint32_t a, uint64_t v) override { wr(a, v); }
    bool MemoryWriteExclusive8(uint32_t a, uint8_t v, uint8_t) override { wr(a, v); return true; }
    bool MemoryWriteExclusive16(uint32_t a, uint16_t v, uint16_t) override { wr(a, v); return true; }
    bool MemoryWriteExclusive32(uint32_t a, uint32_t v, uint32_t) override { wr(a, v); return true; }
    bool MemoryWriteExclusive64(uint32_t a, uint64_t v, uint64_t) override { wr(a, v); return true; }
    void InterpreterFallback(uint32_t pc, size_t) override { std::fprintf(stderr, "guest: interpreter fallback at %08x\n", pc); std::abort(); }
    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        if (e == E::PreloadData || e == E::PreloadDataWithIntentToWrite || e == E::PreloadInstruction || e == E::Yield ||
            e == E::SendEvent || e == E::SendEventLocal || e == E::WaitForEvent || e == E::WaitForInterrupt) return;   // hints
        std::fprintf(stderr, "guest: exception %d at %08x\n", (int)e, pc);
        std::abort();
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 40; }
    void CallSVC(uint32_t n) override {
        if (n == kRetSvc) { jit->HaltExecution(); return; }
        if (n >= thunks.size()) { std::fprintf(stderr, "guest: bad svc %u\n", n); std::abort(); }
        static const bool trace = std::getenv("MPC_HOST_TRACE") != nullptr;   // every thunk call, with the guest thread
        if (trace) std::fprintf(stderr, "[t%u] %s(%x,%x,%x)\n", th.empty() ? 0 : th[cur]->id, thunk_names[n].c_str(), r(0), r(1), r(2));
        thunks[n](*this);
    }

    // --- cooperative guest threads ---
    // One JIT, one running thread at a time. A switch happens inside a thunk by swapping the register file, so the thread that
    // blocks resumes at the `bx lr` after its own svc. A new thread runs first, until it blocks or exits. Threads run only inside a
    // host call(); when the call returns, the runnable workers are drained once (drain()). Sleeping workers wake on the next call.
    enum St { Run, WaitMutex, WaitCond, WaitJoin, Sleep, Done };
    struct GThread {
        uint32_t id = 0; St st = Run;
        std::array<uint32_t, 16> regs{}; std::array<uint32_t, 64> ext{}; uint32_t cpsr = 0x10, fpscr = 0;
        gaddr obj = 0, mtx = 0, join_ret = 0;   // what it waits on, the mutex to take again after a cond wait, where join stores the result
        bool signaled = false, timed = false;
        uint32_t retval = 0, sleep_call = 0;
    };
    std::vector<std::unique_ptr<GThread>> th;
    size_t cur = 0;
    uint32_t call_no = 0, switches = 0;
    bool main_active = false;
    gaddr thread_exit_stub = 0;

    GThread& me() { return *th[cur]; }
    // mutex layout (glibc armhf): count at +4, owner at +8, kind at +12 (1 = recursive)
    bool mutex_free(gaddr mu) { return rd<uint32_t>(mu + 8) == 0; }
    bool try_lock(gaddr mu, uint32_t tid) {
        const uint32_t owner = rd<uint32_t>(mu + 8);
        if (owner == 0) { wr<uint32_t>(mu + 8, tid); wr<uint32_t>(mu + 4, 1); return true; }
        if (owner == tid && rd<uint32_t>(mu + 12) == 1) { wr<uint32_t>(mu + 4, rd<uint32_t>(mu + 4) + 1); return true; }
        return false;
    }
    void unlock(gaddr mu) {
        const uint32_t c = rd<uint32_t>(mu + 4);
        if (c > 1) { wr<uint32_t>(mu + 4, c - 1); return; }
        wr<uint32_t>(mu + 8, 0); wr<uint32_t>(mu + 4, 0);
    }
    bool ready(GThread& t) {
        switch (t.st) {
        case Run: return true;
        case WaitMutex: return mutex_free(t.obj) || (rd<uint32_t>(t.obj + 12) == 1 && rd<uint32_t>(t.obj + 8) == t.id);
        case WaitCond: return t.signaled && mutex_free(t.mtx);
        case WaitJoin: return th[t.obj - 1]->st == Done;
        case Sleep: return call_no > t.sleep_call;
        case Done: return false;
        }
        return false;
    }
    void save(GThread& t) { t.regs = jit->Regs(); t.ext = jit->ExtRegs(); t.cpsr = jit->Cpsr(); t.fpscr = jit->Fpscr(); }
    void load(GThread& t) { jit->Regs() = t.regs; jit->ExtRegs() = t.ext; jit->SetCpsr(t.cpsr); jit->SetFpscr(t.fpscr); jit->ClearExclusiveState(); }
    // The wait ended: take the mutex, store a join result, and make the blocking call return `r0`.
    void finish_wait(GThread& t, uint32_t r0) {
        switch (t.st) {
        case WaitMutex: try_lock(t.obj, t.id); break;
        case WaitCond: try_lock(t.mtx, t.id); break;
        case WaitJoin: if (t.join_ret) wr<uint32_t>(t.join_ret, th[t.obj - 1]->retval); break;
        default: break;
        }
        if (t.st != Run) t.regs[0] = r0;
        t.st = Run; t.signaled = false;
    }
    // Picks the next thread after the current one (round robin), switches to it and returns true; false if none could run.
    // If nothing is ready: advance the clock for sleepers, then (main thread, inside a call) wake the main thread as a timeout.
    bool schedule() {
        const size_t n = th.size();
        for (int pass = 0; pass < 2; pass++) {
            for (size_t k = 1; k <= n; k++) {
                const size_t j = (cur + k) % n;
                if (j == 0 && !main_active) continue;
                GThread& t = *th[j];
                if (t.st == Done || !ready(t)) continue;
                if (j != cur) { save(me()); cur = j; load(t); }
                finish_wait(t, 0);
                if (j != cur || true) jit->Regs()[0] = t.regs[0];
                return true;
            }
            bool sleepers = false;   // time passes only while the main thread waits inside a call; in drain() a sleeper waits for the next call
            for (auto& t : th) if (t->st == Sleep) sleepers = true;
            if (!sleepers || !main_active) break;
            call_no++;
        }
        if (main_active && th[0]->st != Done && th[0]->st != Run) {   // deadlock or a timed wait: the main thread gives up waiting
            GThread& t = *th[0];
            if (t.st == WaitMutex || (t.st == WaitCond && !mutex_free(t.mtx))) { std::fprintf(stderr, "guest: deadlock (main thread waits on a held mutex)\n"); std::abort(); }
            if (cur != 0) { save(me()); cur = 0; load(t); }
            finish_wait(t, t.timed ? 110 : 0);
            return true;
        }
        return false;
    }
    // Blocks the current thread in state `st`; returns once something else ran, or immediately if it is the only thread that can.
    void block(St st, gaddr obj) {
        GThread& t = me();
        t.st = st; t.obj = obj; t.sleep_call = call_no;
        if (!schedule()) { save(t); jit->HaltExecution(); }   // a worker with nothing to do: back to the host
    }
    void yield() {
        if (th.size() < 2) return;
        GThread& t = me(); t.st = Run;
        const size_t before = cur;
        for (size_t k = 1; k < th.size(); k++) {
            const size_t j = (before + k) % th.size();
            if ((j == 0 && !main_active) || th[j]->st == Done || !ready(*th[j])) continue;
            save(t); cur = j; load(*th[j]); finish_wait(*th[j], 0); jit->Regs()[0] = th[j]->regs[0] = (th[j]->regs[0]);
            return;
        }
    }
    uint32_t spawn(gaddr fn, uint32_t arg) {
        auto t = std::make_unique<GThread>();
        t->id = (uint32_t)th.size() + 1;
        const gaddr stack = malloc_(512 * 1024);
        t->regs[0] = arg; t->regs[13] = (stack + 512 * 1024 - 16) & ~7u; t->regs[14] = thread_exit_stub; t->regs[15] = fn & ~1u;
        t->cpsr = 0x10 | ((fn & 1) ? 0x20 : 0);
        const uint32_t id = t->id;
        th.push_back(std::move(t));
        // child first: the parent stays runnable and gets 0 back from pthread_create when the child blocks or exits
        jit->Regs()[0] = 0;
        save(me()); cur = th.size() - 1; load(*th.back());
        return id;
    }
    void exit_thread(uint32_t retval) {
        GThread& t = me(); t.st = Done; t.retval = retval;
        if (!schedule()) { jit->HaltExecution(); }
    }
    // After a host call returns: let workers that can run do so once. Registers the host call needs are put back.
    void drain() {
        if (th.size() < 2) return;
        const auto R = jit->Regs(); const auto E = jit->ExtRegs(); const uint32_t cpsr = jit->Cpsr(), fpscr = jit->Fpscr();
        switches = 0;
        for (int round = 0; round < 1000; round++) {
            size_t w = 0;
            for (size_t j = 1; j < th.size(); j++) if (th[j]->st != Done && ready(*th[j])) { w = j; break; }
            if (!w) break;
            cur = w; load(*th[w]); finish_wait(*th[w], 0); jit->Regs()[0] = th[w]->regs[0];
            depth++; jit->Run(); jit->ClearHalt(Dynarmic::HaltReason::UserDefined1); depth--;
        }
        cur = 0;
        jit->Regs() = R; jit->ExtRegs() = E; jit->SetCpsr(cpsr); jit->SetFpscr(fpscr);
    }

    // --- argument access (armhf AAPCS) ---
    uint32_t& r(int i) { return jit->Regs()[i]; }
    uint32_t arg(int i) { return i < 4 ? r(i) : rd<uint32_t>(r(13) + 4 * (i - 4)); }
    float s(int i) { float f; std::memcpy(&f, &jit->ExtRegs()[i], 4); return f; }
    double d(int i) { uint64_t u = jit->ExtRegs()[2 * i] | (uint64_t)jit->ExtRegs()[2 * i + 1] << 32; double v; std::memcpy(&v, &u, 8); return v; }
    void ret(uint32_t v) { r(0) = v; }
    void ret64(uint64_t v) { r(0) = (uint32_t)v; r(1) = (uint32_t)(v >> 32); }
    void ret_f(float f) { std::memcpy(&jit->ExtRegs()[0], &f, 4); }
    void ret_d(double v) { uint64_t u; std::memcpy(&u, &v, 8); jit->ExtRegs()[0] = (uint32_t)u; jit->ExtRegs()[1] = (uint32_t)(u >> 32); }
    char* str(gaddr a) { return reinterpret_cast<char*>(m + a); }

    gaddr add_thunk(const std::string& name, Thunk t) {
        const uint32_t n = (uint32_t)thunks.size();
        const gaddr at = kThunks + 8 * n;
        if (at + 8 > kData) { std::fprintf(stderr, "guest: thunk page full\n"); std::abort(); }
        wr<uint32_t>(at, 0xEF000000u | n);   // svc #n
        wr<uint32_t>(at + 4, 0xE12FFF1Eu);   // bx lr
        thunks.push_back(std::move(t));
        thunk_names.push_back(name);
        return at;
    }
    gaddr thunk_for(const std::string& name, const Thunk& t) {
        auto it = thunk_by_name.find(name);
        if (it != thunk_by_name.end()) return it->second;
        return thunk_by_name[name] = add_thunk(name, t);
    }
    gaddr stub(const std::vector<uint32_t>& code) {   // ARM code in guest memory
        const gaddr a = data((uint32_t)code.size() * 4, 4);
        for (size_t k = 0; k < code.size(); k++) wr<uint32_t>(a + 4 * (uint32_t)k, code[k]);
        return a;
    }
    gaddr data(uint32_t size, uint32_t align = 8) {
        data_next = (data_next + align - 1) & ~(align - 1);
        const gaddr a = data_next;
        data_next += size;
        return a;
    }

    // --- guest heap: size classes, 16-byte aligned ---
    static uint32_t cls(uint32_t n) { uint32_t c = 16; while (c < n) c <<= 1; return c; }
    gaddr malloc_(uint32_t n) {
        const uint32_t c = cls(n ? n : 1);
        auto& fl = free_lists[c];
        gaddr p;
        if (!fl.empty()) { p = fl.back(); fl.pop_back(); }
        else {
            if (heap_next + c > kHeapEnd) return 0;
            p = heap_next; heap_next += c;
        }
        sizes[p] = c;
        return p;
    }
    void free_(gaddr p) {
        if (!p) return;
        auto it = sizes.find(p);
        if (it == sizes.end()) { std::fprintf(stderr, "guest: free of unknown %08x\n", p); return; }
        free_lists[it->second].push_back(p);
        sizes.erase(it);
    }

    // --- printf: format from guest memory with guest varargs ---
    struct VarArgs {
        Impl& i; int reg; uint32_t sp;
        uint32_t next32() { if (reg < 4) return i.r(reg++); const uint32_t v = i.rd<uint32_t>(sp); sp += 4; return v; }
        uint64_t next64() {
            if (reg < 4) { if (reg & 1) reg++; if (reg < 4) { const uint64_t v = i.r(reg) | (uint64_t)i.r(reg + 1) << 32; reg += 2; return v; } }
            sp = (sp + 7) & ~7u; const uint64_t v = i.rd<uint64_t>(sp); sp += 8; return v;
        }
    };
    std::string format(const char* f, VarArgs va) {
        std::string out;
        char tmp[512];
        while (*f) {
            if (*f != '%') { out += *f++; continue; }
            const char* start = f++;
            if (*f == '%') { out += '%'; f++; continue; }
            std::string spec = "%";
            while (std::strchr("-+ #0", *f)) spec += *f++;
            if (*f == '*') { spec += std::to_string((int32_t)va.next32()); f++; } else while (std::isdigit((unsigned char)*f)) spec += *f++;
            if (*f == '.') { spec += *f++; if (*f == '*') { spec += std::to_string((int32_t)va.next32()); f++; } else while (std::isdigit((unsigned char)*f)) spec += *f++; }
            int lng = 0;   // 0 int, 2 long long
            while (std::strchr("hlLzjt", *f)) { if (*f == 'l' && f[1] == 'l') { lng = 2; f += 2; } else { if (*f == 'L') lng = 3; f++; } }
            const char c = *f++;
            spec += (lng == 2 && std::strchr("diouxX", c)) ? std::string("ll") + c : std::string(1, c);
            switch (c) {
            case 'd': case 'i': if (lng == 2) std::snprintf(tmp, sizeof tmp, spec.c_str(), (long long)va.next64()); else std::snprintf(tmp, sizeof tmp, spec.c_str(), (int32_t)va.next32()); break;
            case 'o': case 'u': case 'x': case 'X': if (lng == 2) std::snprintf(tmp, sizeof tmp, spec.c_str(), (unsigned long long)va.next64()); else std::snprintf(tmp, sizeof tmp, spec.c_str(), va.next32()); break;
            case 'c': std::snprintf(tmp, sizeof tmp, spec.c_str(), (int)va.next32()); break;
            case 's': { const uint32_t p = va.next32(); std::snprintf(tmp, sizeof tmp, spec.c_str(), p ? str(p) : "(null)"); break; }
            case 'p': std::snprintf(tmp, sizeof tmp, "0x%x", va.next32()); break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                const uint64_t u = va.next64(); double v; std::memcpy(&v, &u, 8);
                if (lng == 3) spec.erase(spec.size() - 1, 1), spec += c;   // long double == double on armhf
                std::snprintf(tmp, sizeof tmp, spec.c_str(), v); break;
            }
            default: out.append(start, f); continue;
            }
            out += tmp;
        }
        return out;
    }

    size_t scan_used = 0;
    // sscanf: %d %i %u %x %o %f %g %e %s %c %n %% and literals, the l modifier, and * to skip. Field widths limit the input read.
    // Returns the number of stores, or -1 (EOF) if the input ends before the first conversion.
    int scan(const char* in, const char* f, VarArgs va) {
        int n = 0; const char* s = in;
        struct Used { size_t& u; const char*& s; const char* in; ~Used() { u = (size_t)(s - in); } } used_guard{scan_used, s, in};   // how much input was consumed
        while (*f) {
            if (std::isspace((unsigned char)*f)) { while (std::isspace((unsigned char)*s)) s++; f++; continue; }
            if (*f != '%') { if (*s != *f) return (!*s && !n) ? -1 : n; s++; f++; continue; }
            f++;
            if (*f == '%') { if (*s != '%') return n; s++; f++; continue; }
            bool skip = false; if (*f == '*') { skip = true; f++; }
            uint32_t width = 0;
            while (std::isdigit((unsigned char)*f)) width = width * 10 + (uint32_t)(*f++ - '0');
            int lng = 0;
            while (*f && std::strchr("hlLzjt", *f)) { if (*f == 'l') lng++; f++; }
            const char c = *f++;
            if (c == 'n') { if (!skip) wr<uint32_t>(va.next32(), (uint32_t)(s - in)); continue; }
            if (c != 'c') while (std::isspace((unsigned char)*s)) s++;
            if (!*s) return n ? n : -1;
            char field[128];   // the input limited to the field width, for the numeric conversions
            if (c != 'c' && c != 's') { std::strncpy(field, s, width && width < sizeof field ? width : sizeof field - 1); field[width && width < sizeof field ? width : sizeof field - 1] = 0; }
            char* e = const_cast<char*>(s);
            switch (c) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                const int base = c == 'x' || c == 'X' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10;
                const long v = (c == 'u' || base == 16 || base == 8) ? (long)std::strtoul(field, &e, base) : std::strtol(field, &e, base);
                if (e == field) return n;
                e = const_cast<char*>(s) + (e - field);
                if (!skip) wr<uint32_t>(va.next32(), (uint32_t)v);
                break; }
            case 'f': case 'g': case 'e': case 'a': case 'E': case 'G': {
                const double v = std::strtod(field, &e);
                if (e == field) return n;
                e = const_cast<char*>(s) + (e - field);
                if (!skip) { const gaddr p = va.next32(); if (lng) std::memcpy(m + p, &v, 8); else { const float fv = (float)v; std::memcpy(m + p, &fv, 4); } }
                break; }
            case 's': {
                while (*e && !std::isspace((unsigned char)*e) && (!width || (uint32_t)(e - s) < width)) e++;
                if (!skip) { const gaddr p = va.next32(); std::memcpy(m + p, s, (size_t)(e - s)); m[p + (e - s)] = 0; }
                break; }
            case 'c':
                e = const_cast<char*>(s) + 1;
                if (!skip) m[va.next32()] = (uint8_t)*s;
                break;
            default: return n;
            }
            s = e;
            if (!skip) n++;
        }
        return n;
    }
    void put(gaddr buf, uint32_t n, const std::string& s) {
        if (n) { const size_t k = std::min<size_t>(s.size(), n - 1); std::memcpy(m + buf, s.data(), k); m[buf + k] = 0; }
        ret((uint32_t)s.size());
    }

    void install_libc();
};

// Host libc, libm and libstdc++ for the guest. Signatures follow armhf (floats/doubles in VFP registers).
void Guest::Impl::install_libc() {}

namespace {
using I = Guest::Impl;
struct Entry { const char* name; Thunk fn; };

// MPC_HOST_LAX=1: an unsupported import logs once and returns 0 instead of stopping (to list what a plugin really calls).
void abort_named(I& i, const std::string& n) {
    static const bool lax = std::getenv("MPC_HOST_LAX") != nullptr;
    static std::unordered_map<std::string, int> seen;
    if (lax && n.find("stack smashed") == std::string::npos && n != "abort") { if (!seen[n]++) std::fprintf(stderr, "guest: stub %s, lr=%08x\n", n.c_str(), i.r(14)); i.ret(0); return; }
    std::fprintf(stderr, "guest: called %s (not supported), lr=%08x\n", n.c_str(), i.r(14));
    std::abort();
}

std::vector<Entry> libc_table() {
    std::vector<Entry> t;
    auto add = [&](const char* n, Thunk f) { t.push_back({n, std::move(f)}); };
    // memory and strings
    add("malloc", [](I& i) { i.ret(i.malloc_(i.r(0))); });
    add("calloc", [](I& i) { const uint64_t n = (uint64_t)i.r(0) * i.r(1); const gaddr p = n > 0xffffffff ? 0 : i.malloc_((uint32_t)n); if (p) std::memset(i.m + p, 0, (size_t)n); i.ret(p); });
    add("realloc", [](I& i) {
        const gaddr old = i.r(0); const uint32_t n = i.r(1);
        const gaddr p = i.malloc_(n);
        if (old && p) { std::memcpy(i.m + p, i.m + old, std::min(n, i.sizes[old])); i.free_(old); }
        i.ret(p);
    });
    add("free", [](I& i) { i.free_(i.r(0)); });
    add("memcpy", [](I& i) { std::memcpy(i.m + i.r(0), i.m + i.r(1), i.r(2)); });
    add("memmove", [](I& i) { std::memmove(i.m + i.r(0), i.m + i.r(1), i.r(2)); });
    add("memset", [](I& i) { std::memset(i.m + i.r(0), (int)i.r(1), i.r(2)); });
    add("memcmp", [](I& i) { i.ret((uint32_t)std::memcmp(i.m + i.r(0), i.m + i.r(1), i.r(2))); });
    add("strlen", [](I& i) { i.ret((uint32_t)std::strlen(i.str(i.r(0)))); });
    add("strcmp", [](I& i) { i.ret((uint32_t)std::strcmp(i.str(i.r(0)), i.str(i.r(1)))); });
    add("strncmp", [](I& i) { i.ret((uint32_t)std::strncmp(i.str(i.r(0)), i.str(i.r(1)), i.r(2))); });
    add("strcasecmp", [](I& i) { i.ret((uint32_t)::strcasecmp(i.str(i.r(0)), i.str(i.r(1)))); });
    add("strncpy", [](I& i) { std::strncpy(i.str(i.r(0)), i.str(i.r(1)), i.r(2)); });
    add("strcpy", [](I& i) { std::strcpy(i.str(i.r(0)), i.str(i.r(1))); });
    add("strchr", [](I& i) { const char* p = std::strchr(i.str(i.r(0)), (int)i.r(1)); i.ret(p ? (uint32_t)(reinterpret_cast<const uint8_t*>(p) - i.m) : 0); });
    add("strrchr", [](I& i) { const char* p = std::strrchr(i.str(i.r(0)), (int)i.r(1)); i.ret(p ? (uint32_t)(reinterpret_cast<const uint8_t*>(p) - i.m) : 0); });
    add("strstr", [](I& i) { const char* p = std::strstr(i.str(i.r(0)), i.str(i.r(1))); i.ret(p ? (uint32_t)(reinterpret_cast<const uint8_t*>(p) - i.m) : 0); });
    auto strtol_ = [](I& i) { char* e; const long v = std::strtol(i.str(i.r(0)), &e, (int)i.r(2)); if (i.r(1)) i.wr<uint32_t>(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret((uint32_t)(int32_t)v); };
    add("strtol", strtol_);
    add("__isoc23_strtol", strtol_);
    add("strtoul", [](I& i) { char* e; const unsigned long v = std::strtoul(i.str(i.r(0)), &e, (int)i.r(2)); if (i.r(1)) i.wr<uint32_t>(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret((uint32_t)v); });
    add("__isoc23_strtoul", t.back().fn);
    add("strtod", [](I& i) { char* e; const double v = std::strtod(i.str(i.r(0)), &e); if (i.r(1)) i.wr<uint32_t>(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_d(v); });
    add("strtof", [](I& i) { char* e; const float v = std::strtof(i.str(i.r(0)), &e); if (i.r(1)) i.wr<uint32_t>(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_f(v); });
    add("atof", [](I& i) { i.ret_d(std::atof(i.str(i.r(0)))); });
    add("atoi", [](I& i) { i.ret((uint32_t)std::atoi(i.str(i.r(0)))); });
    add("getenv", [](I& i) { i.ret(0); });   // no environment for guests
    // scanf family (strings only; no stdin or files)
    auto sscanf_ = [](I& i) { i.ret((uint32_t)i.scan(i.str(i.r(0)), i.str(i.r(1)), {i, 2, i.r(13)})); };
    add("sscanf", sscanf_);
    add("__isoc99_sscanf", sscanf_);
    add("__isoc23_sscanf", sscanf_);
    // printf family
    add("snprintf", [](I& i) { const gaddr b = i.r(0); const uint32_t n = i.r(1); i.put(b, n, i.format(i.str(i.r(2)), {i, 3, i.r(13)})); });
    add("__snprintf_chk", [](I& i) { const gaddr b = i.r(0); const uint32_t n = i.r(1); i.put(b, n, i.format(i.str(i.arg(4)), {i, 4, i.r(13) + 4})); });
    add("sprintf", [](I& i) { const gaddr b = i.r(0); i.put(b, 0x7fffffff, i.format(i.str(i.r(1)), {i, 2, i.r(13)})); });
    add("__sprintf_chk", [](I& i) { const gaddr b = i.r(0); i.put(b, 0x7fffffff, i.format(i.str(i.r(3)), {i, 4, i.r(13)})); });
    add("puts", [](I& i) { std::fprintf(stderr, "[guest] %s\n", i.str(i.r(0))); i.ret(1); });
    add("printf", [](I& i) { const std::string s = i.format(i.str(i.r(0)), {i, 1, i.r(13)}); std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    add("__printf_chk", [](I& i) { const std::string s = i.format(i.str(i.r(1)), {i, 2, i.r(13)}); std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    // time
    add("__clock_gettime64", [](I& i) { timespec ts; clock_gettime((clockid_t)i.r(0), &ts); const gaddr p = i.r(1); i.wr<int64_t>(p, ts.tv_sec); i.wr<int32_t>(p + 8, (int32_t)ts.tv_nsec); i.wr<int32_t>(p + 12, 0); i.ret(0); });
    add("clock_gettime", [](I& i) { timespec ts; clock_gettime((clockid_t)i.r(0), &ts); const gaddr p = i.r(1); i.wr<int32_t>(p, (int32_t)ts.tv_sec); i.wr<int32_t>(p + 4, (int32_t)ts.tv_nsec); i.ret(0); });
    // threads: one guest thread, so locks are no-ops
    add("pthread_mutex_init", [](I& i) { std::memset(i.m + i.r(0), 0, 24); if (i.r(1)) i.wr<uint32_t>(i.r(0) + 12, i.rd<uint32_t>(i.r(1))); i.ret(0); });
    add("pthread_mutexattr_init", [](I& i) { i.wr<uint32_t>(i.r(0), 0); i.ret(0); });
    add("pthread_mutexattr_settype", [](I& i) { i.wr<uint32_t>(i.r(0), i.r(1)); i.ret(0); });
    for (const char* n : {"pthread_mutex_destroy", "pthread_mutexattr_destroy", "pthread_mutexattr_setprotocol"}) add(n, [](I& i) { i.ret(0); });
    add("pthread_mutex_lock", [](I& i) { const gaddr mu = i.r(0); if (i.try_lock(mu, i.me().id)) { i.ret(0); return; } i.ret(0); i.block(I::WaitMutex, mu); });
    add("pthread_mutex_trylock", [](I& i) { i.ret(i.try_lock(i.r(0), i.me().id) ? 0 : 16); });
    add("pthread_mutex_unlock", [](I& i) { i.unlock(i.r(0)); i.ret(0); });
    auto cond_wait = [](I& i, bool timed) {
        const gaddr c = i.r(0), mu = i.r(1);
        i.ret(0);
        i.unlock(mu);
        i.me().mtx = mu; i.me().signaled = false; i.me().timed = timed;
        i.block(I::WaitCond, c);
    };
    add("pthread_cond_wait", [cond_wait](I& i) { cond_wait(i, false); });
    add("pthread_cond_timedwait", [cond_wait](I& i) { cond_wait(i, true); });
    add("pthread_cond_clockwait", [cond_wait](I& i) { cond_wait(i, true); });
    add("pthread_cond_signal", [](I& i) { for (auto& t : i.th) if (t->st == I::WaitCond && t->obj == i.r(0) && !t->signaled) { t->signaled = true; break; } i.ret(0); });
    add("pthread_cond_broadcast", [](I& i) { for (auto& t : i.th) if (t->st == I::WaitCond && t->obj == i.r(0)) t->signaled = true; i.ret(0); });
    add("pthread_create", [](I& i) { const gaddr thr = i.r(0), fn = i.r(2), arg = i.r(3); const uint32_t id = i.th.size() + 1; i.wr<uint32_t>(thr, id); i.spawn(fn, arg); });
    add("pthread_join", [](I& i) {
        const uint32_t t = i.r(0); if (t < 1 || t > i.th.size()) { i.ret(3); return; }   // ESRCH
        i.me().join_ret = i.r(1); i.ret(0);
        i.block(I::WaitJoin, t); });
    add("pthread_self", [](I& i) { i.ret(i.me().id); });
    add("sched_yield", [](I& i) { i.ret(0); i.yield(); });
    for (const char* n : {"usleep", "sleep"}) add(n, [](I& i) { i.ret(0); i.block(I::Sleep, 0); });
    add("nanosleep", [](I& i) { i.ret(0); i.block(I::Sleep, 0); });
    add("clock_nanosleep", [](I& i) { i.ret(0); i.block(I::Sleep, 0); });
    add("poll", [](I& i) { i.ret(0); if (i.r(2) != 0) i.block(I::Sleep, 0); });
    // process
    add("raise", [](I& i) { std::fprintf(stderr, "guest: raise(%u)\n", i.r(0)); std::abort(); });
    add("abort", [](I& i) { abort_named(i, "abort"); });
    add("__stack_chk_fail", [](I& i) { abort_named(i, "__stack_chk_fail (stack smashed)"); });
    add("__cxa_finalize", [](I&) {});
    add("__cxa_atexit", [](I& i) { i.ret(0); });
    add("__aeabi_atexit", [](I& i) { i.ret(0); });
    add("__memset_chk", [](I& i) { std::memset(i.m + i.r(0), (int)i.r(1), i.r(2)); });
    // pthread keys (one thread): a key is an index into a value table
    add("pthread_key_create", [](I& i) { i.wr<uint32_t>(i.r(0), (uint32_t)i.keys.size()); i.keys.push_back(0); i.ret(0); });
    add("pthread_key_delete", [](I& i) { i.ret(0); });
    add("pthread_getspecific", [](I& i) { i.ret(i.r(0) < i.keys.size() ? i.keys[i.r(0)] : 0); });
    add("pthread_setspecific", [](I& i) { if (i.r(0) < i.keys.size()) i.keys[i.r(0)] = i.r(1); i.ret(0); });
    // --- what libstdc++ (loaded as a guest library) asks of libc: the "C" locale only, one thread, no real files ---
    add("newlocale", [](I& i) { i.ret(i.r(2) ? i.r(2) : i.c_locale()); });
    add("__newlocale", [](I& i) { i.ret(i.r(2) ? i.r(2) : i.c_locale()); });
    add("duplocale", [](I& i) { i.ret(i.c_locale()); });
    add("__duplocale", [](I& i) { i.ret(i.c_locale()); });
    add("freelocale", [](I& i) { i.ret(0); });
    add("__freelocale", [](I& i) { i.ret(0); });
    add("uselocale", [](I& i) { i.ret(i.c_locale()); });
    add("__uselocale", [](I& i) { i.ret(i.c_locale()); });
    add("setlocale", [](I& i) { i.ret(i.cstr("C")); });
    add("nl_langinfo_l", [](I& i) { i.ret(i.cstr("")); });
    add("__nl_langinfo_l", [](I& i) { i.ret(i.cstr("")); });
    add("__ctype_get_mb_cur_max", [](I& i) { i.ret(1); });
    add("__strtod_l", [](I& i) { char* e; const double v = std::strtod(i.str(i.r(0)), &e); if (i.r(1)) i.wr<uint32_t>(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_d(v); });
    add("strtold_l", [](I& i) { char* e; const double v = std::strtod(i.str(i.r(0)), &e); if (i.r(1)) i.wr<uint32_t>(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_d(v); });
    add("__strtof_l", [](I& i) { char* e; const float v = std::strtof(i.str(i.r(0)), &e); if (i.r(1)) i.wr<uint32_t>(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_f(v); });
    add("__strcoll_l", [](I& i) { i.ret((uint32_t)std::strcmp(i.str(i.r(0)), i.str(i.r(1)))); });
    add("__strxfrm_l", [](I& i) { const size_t n = std::strlen(i.str(i.r(1))); if (i.r(2) > n) std::strcpy(i.str(i.r(0)), i.str(i.r(1))); i.ret((uint32_t)n); });
    add("isspace", [](I& i) { i.ret(std::isspace((int)i.r(0)) ? 1 : 0); });
    add("__errno_location", [](I& i) { if (!i.errno_p) i.errno_p = i.data(4, 4); i.ret(i.errno_p); });
    add("strerror", [](I& i) { i.ret(i.cstr("error")); });
    add("__assert_fail", [](I& i) { std::fprintf(stderr, "guest: assert %s at %s:%u\n", i.str(i.r(0)), i.str(i.r(1)), i.r(2)); std::abort(); });
    add("syscall", [](I& i) { abort_named(i, "syscall"); });
    for (const char* n : {"pthread_cond_destroy", "pthread_cond_init", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
                          "pthread_rwlock_unlock", "pthread_detach", "__cxa_thread_atexit_impl"})
        add(n, [](I& i) { i.ret(0); });   // one thread: nothing to wait for or wake
    add("memchr", [](I& i) { const void* p = std::memchr(i.m + i.r(0), (int)i.r(1), i.r(2)); i.ret(p ? (uint32_t)(static_cast<const uint8_t*>(p) - i.m) : 0); });
    add("strdup", [](I& i) { i.ret(i.g.strdup(i.str(i.r(0)))); });
    add("posix_memalign", [](I& i) { const gaddr p = i.malloc_(i.r(2) + i.r(1)); i.wr<uint32_t>(i.r(0), (p + i.r(1) - 1) & ~(i.r(1) - 1)); i.ret(0); });
    add("__memcpy_chk", [](I& i) { std::memcpy(i.m + i.r(0), i.m + i.r(1), i.r(2)); i.ret(i.r(0)); });
    add("__memmove_chk", [](I& i) { std::memmove(i.m + i.r(0), i.m + i.r(1), i.r(2)); i.ret(i.r(0)); });
    add("__strcpy_chk", [](I& i) { std::strcpy(i.str(i.r(0)), i.str(i.r(1))); i.ret(i.r(0)); });
    add("wcslen", [](I& i) { uint32_t n = 0; while (i.rd<uint32_t>(i.r(0) + 4 * n)) n++; i.ret(n); });
    add("frexp", [](I& i) { int e; const double v = std::frexp(i.d(0), &e); i.wr<int32_t>(i.r(0), e); i.ret_d(v); });
    add("ldexp", [](I& i) { i.ret_d(std::ldexp(i.d(0), (int)i.r(0))); });
    add("ldexpf", [](I& i) { i.ret_f(std::ldexp(i.s(0), (int)i.r(0))); });
    add("modf", [](I& i) { double ip; const double v = std::modf(i.d(0), &ip); std::memcpy(i.m + i.r(0), &ip, 8); i.ret_d(v); });
    add("vsnprintf", [](I& i) { const gaddr b = i.r(0); const uint32_t n = i.r(1); i.put(b, n, i.format(i.str(i.r(2)), {i, 4, i.r(3)})); });   // armhf va_list = one pointer
    // wide characters: ASCII only (the "C" locale)
    add("wctob", [](I& i) { i.ret(i.r(0) < 128 ? i.r(0) : 0xFFFFFFFFu); });
    add("btowc", [](I& i) { i.ret(i.r(0) < 128 ? i.r(0) : 0xFFFFFFFFu); });
    add("mbrtowc", [](I& i) {
        if (!i.r(1)) { i.ret(0); return; }
        if (!i.r(2)) { i.ret(0xFFFFFFFEu); return; }
        const uint8_t c = i.m[i.r(1)];
        if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
        if (i.r(0)) i.wr<uint32_t>(i.r(0), c);
        i.ret(c ? 1 : 0); });
    add("wcrtomb", [](I& i) { if (!i.r(0)) { i.ret(1); return; } if (i.r(1) >= 128) { i.ret(0xFFFFFFFFu); return; } i.m[i.r(0)] = (uint8_t)i.r(1); i.ret(1); });
    add("mbsnrtowcs", [](I& i) {
        const gaddr dst = i.r(0), src = i.r(1), nms = i.r(2), len = i.r(3); const gaddr s0 = i.rd<uint32_t>(src);
        uint32_t n = 0;
        for (; n < nms && (!dst || n < len); n++) {
            const uint8_t c = i.m[s0 + n];
            if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
            if (dst) i.wr<uint32_t>(dst + 4 * n, c);
            if (!c) { if (dst) i.wr<uint32_t>(src, 0); i.ret(n); return; }
        }
        if (dst) i.wr<uint32_t>(src, s0 + n);
        i.ret(n); });
    add("__mbsrtowcs_chk", [](I& i) {
        const gaddr dst = i.r(0), src = i.r(1), len = i.r(2); const gaddr s0 = i.rd<uint32_t>(src);
        uint32_t n = 0;
        for (;; n++) {
            if (dst && n >= len) break;
            const uint8_t c = i.m[s0 + n];
            if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
            if (dst) i.wr<uint32_t>(dst + 4 * n, c);
            if (!c) { if (dst) i.wr<uint32_t>(src, 0); i.ret(n); return; }
        }
        i.wr<uint32_t>(src, s0 + n); i.ret(n); });
    add("wcsnrtombs", [](I& i) {
        const gaddr dst = i.r(0), src = i.r(1), nwc = i.r(2), len = i.r(3); const gaddr s0 = i.rd<uint32_t>(src);
        uint32_t n = 0;
        for (; n < nwc && (!dst || n < len); n++) {
            const uint32_t c = i.rd<uint32_t>(s0 + 4 * n);
            if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
            if (dst) i.m[dst + n] = (uint8_t)c;
            if (!c) { if (dst) i.wr<uint32_t>(src, 0); i.ret(n); return; }
        }
        if (dst) i.wr<uint32_t>(src, s0 + 4 * n);
        i.ret(n); });
    add("wmemchr", [](I& i) { for (uint32_t k = 0; k < i.r(2); k++) if (i.rd<uint32_t>(i.r(0) + 4 * k) == i.r(1)) { i.ret(i.r(0) + 4 * k); return; } i.ret(0); });
    add("wmemcmp", [](I& i) { for (uint32_t k = 0; k < i.r(2); k++) { const uint32_t a = i.rd<uint32_t>(i.r(0) + 4 * k), b = i.rd<uint32_t>(i.r(1) + 4 * k); if (a != b) { i.ret(a < b ? 0xFFFFFFFFu : 1); return; } } i.ret(0); });
    add("wmemcpy", [](I& i) { std::memcpy(i.m + i.r(0), i.m + i.r(1), 4 * (size_t)i.r(2)); i.ret(i.r(0)); });
    add("__wmemcpy_chk", [](I& i) { std::memcpy(i.m + i.r(0), i.m + i.r(1), 4 * (size_t)i.r(2)); i.ret(i.r(0)); });
    add("wmemmove", [](I& i) { std::memmove(i.m + i.r(0), i.m + i.r(1), 4 * (size_t)i.r(2)); i.ret(i.r(0)); });
    add("wmemset", [](I& i) { for (uint32_t k = 0; k < i.r(2); k++) i.wr<uint32_t>(i.r(0) + 4 * k, i.r(1)); i.ret(i.r(0)); });
    add("__wmemset_chk", [](I& i) { for (uint32_t k = 0; k < i.r(2); k++) i.wr<uint32_t>(i.r(0) + 4 * k, i.r(1)); i.ret(i.r(0)); });
    add("wcscmp", [](I& i) { for (uint32_t k = 0;; k++) { const uint32_t a = i.rd<uint32_t>(i.r(0) + 4 * k), b = i.rd<uint32_t>(i.r(1) + 4 * k); if (a != b) { i.ret(a < b ? 0xFFFFFFFFu : 1); return; } if (!a) { i.ret(0); return; } } });
    add("__wcscoll_l", [](I& i) { for (uint32_t k = 0;; k++) { const uint32_t a = i.rd<uint32_t>(i.r(0) + 4 * k), b = i.rd<uint32_t>(i.r(1) + 4 * k); if (a != b) { i.ret(a < b ? 0xFFFFFFFFu : 1); return; } if (!a) { i.ret(0); return; } } });
    add("__towlower_l", [](I& i) { const uint32_t c = i.r(0); i.ret(c < 128 ? (uint32_t)std::tolower((int)c) : c); });
    add("__towupper_l", [](I& i) { const uint32_t c = i.r(0); i.ret(c < 128 ? (uint32_t)std::toupper((int)c) : c); });
    add("__wctype_l", [](I& i) { i.ret((uint32_t)std::wctype(i.str(i.r(0)))); });
    add("__iswctype_l", [](I& i) { i.ret(i.r(0) < 128 ? (uint32_t)std::iswctype(i.r(0), (wctype_t)i.r(1)) : 0); });
    add("secure_getenv", [](I& i) { i.ret(0); });
    // no filesystem for guests: opens and reads fail the way a missing file would
    for (const char* n : {"setvbuf"}) add(n, [](I& i) { i.ret(0); });
    for (const char* n : {"symlink", "link", "chown", "chmod", "fchmod", "fchmodat", "truncate64"}) add(n, [](I& i) { i.ret(0xFFFFFFFFu); });
    add("rand", [](I& i) { i.ret((uint32_t)std::rand()); });
    add("srand", [](I& i) { std::srand(i.r(0)); });
    add("time", [](I& i) { const uint32_t t = (uint32_t)std::time(nullptr); if (i.r(0)) i.wr<uint32_t>(i.r(0), t); i.ret(t); });
    // single-import stoppers found by the survey: calls that can fail or do nothing for a sandboxed guest
    add("frexpf", [](I& i) { int e; const float v = std::frexp(i.s(0), &e); i.wr<int32_t>(i.r(0), e); i.ret_f(v); });
    add("fma", [](I& i) { i.ret_d(std::fma(i.d(0), i.d(1), i.d(2))); });
    add("fmaf", [](I& i) { i.ret_f(std::fma(i.s(0), i.s(1), i.s(2))); });
    add("localtime", [](I& i) { i.ret(0); });   // callers log timestamps: a NULL result is allowed by the C standard
    for (const char* n : {"dlopen", "dlsym", "dlerror"}) add(n, [](I& i) { i.ret(0); });
    for (const char* n : {"sigaction", "pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setschedpolicy", "pthread_attr_setschedparam",
                          "pthread_attr_setinheritsched", "pthread_setname_np", "pthread_setaffinity_np", "sched_setaffinity", "pthread_setschedparam"})
        add(n, [](I& i) { i.ret(0); });
    for (const char* n : {"getrlimit", "snd_seq_open"}) add(n, [](I& i) { i.ret(0xFFFFFFFFu); });
    add("strtok_r", [](I& i) {
        gaddr p = i.r(0) ? i.r(0) : i.rd<uint32_t>(i.r(2));
        const char* delim = i.str(i.r(1));
        if (!p) { i.ret(0); return; }
        while (i.m[p] && std::strchr(delim, i.m[p])) p++;
        if (!i.m[p]) { i.wr<uint32_t>(i.r(2), p); i.ret(0); return; }
        const gaddr tok = p;
        while (i.m[p] && !std::strchr(delim, i.m[p])) p++;
        if (i.m[p]) { i.m[p] = 0; p++; }
        i.wr<uint32_t>(i.r(2), p);
        i.ret(tok); });
    add("strftime", [](I& i) { if (i.r(1)) i.m[i.r(0)] = 0; i.ret(0); });   // no clock formatting: an empty string
    for (const char* n : {"setrlimit", "pthread_attr_setstacksize", "pthread_attr_setdetachstate"}) add(n, [](I& i) { i.ret(0); });
    add("aligned_alloc", [](I& i) { const gaddr p = i.malloc_(i.r(1) + i.r(0)); i.ret((p + i.r(0) - 1) & ~(i.r(0) - 1)); });
    add("setenv", [](I& i) { i.ret(0); });
    add("bcmp", [](I& i) { i.ret((uint32_t)std::memcmp(i.m + i.r(0), i.m + i.r(1), i.r(2))); });
    for (const char* n : {"pthread_getschedparam", "pthread_getattr_np", "pthread_attr_getstack", "pthread_attr_getguardsize", "sched_getaffinity"}) add(n, [](I& i) { i.ret(0xFFFFFFFFu); });
    for (const char* n : {"socketpair", "socket", "pipe"}) add(n, [](I& i) { i.ret(0xFFFFFFFFu); });
    add("gettimeofday", [](I& i) { timeval tv; gettimeofday(&tv, nullptr); if (i.r(0)) { i.wr<int32_t>(i.r(0), (int32_t)tv.tv_sec); i.wr<int32_t>(i.r(0) + 4, (int32_t)tv.tv_usec); } i.ret(0); });
    // ARM EHABI: the unwinder asks which exception index table covers a pc (glibc provides this, built on dl_iterate_phdr)
    add("__gnu_Unwind_Find_exidx", [](I& i) {
        for (auto& md : i.mods) if (i.r(0) >= md.base && i.r(0) < md.end && md.exidx) { if (i.r(1)) i.wr<uint32_t>(i.r(1), md.exidx_size / 8); i.ret(md.exidx); return; }
        if (i.r(1)) i.wr<uint32_t>(i.r(1), 0);
        i.ret(0); });
    // --- files (see Impl::hostpath): stdio on host FILE*s, fds are host fds, directories, stat, glob ---
    auto out_file = [](I& i, gaddr f) -> FILE* { FILE* h = i.file_of(f); return h ? h : stderr; };
    auto fopen_ = [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.set_errno(2); i.ret(0); return; }
        FILE* f = std::fopen(hp.c_str(), i.str(i.r(1)));
        if (!f) { i.set_errno(errno); i.ret(0); return; }
        const gaddr h = i.malloc_(16); i.files[h] = f; i.ret(h); };
    add("fopen", fopen_); add("fopen64", fopen_);
    add("fclose", [](I& i) { FILE* f = i.file_of(i.r(0)); if (!f) { i.ret(0xFFFFFFFFu); return; } const int r = std::fclose(f); i.files.erase(i.r(0)); i.free_(i.r(0)); i.ret((uint32_t)r); });
    add("fflush", [](I& i) { if (FILE* f = i.file_of(i.r(0))) std::fflush(f); i.ret(0); });
    add("fread", [](I& i) { FILE* f = i.file_of(i.r(3)); i.ret(f ? (uint32_t)std::fread(i.m + i.r(0), i.r(1), i.r(2), f) : 0); });
    add("__fread_chk", [](I& i) { FILE* f = i.file_of(i.arg(4)); i.ret(f ? (uint32_t)std::fread(i.m + i.r(0), i.r(2), i.r(3), f) : 0); });
    add("fwrite", [](I& i) { FILE* f = i.file_of(i.r(3)); if (f) i.ret((uint32_t)std::fwrite(i.m + i.r(0), i.r(1), i.r(2), f)); else { std::fwrite(i.m + i.r(0), i.r(1), i.r(2), stderr); i.ret(i.r(2)); } });
    add("fgets", [](I& i) { FILE* f = i.file_of(i.r(2)); i.ret(f && std::fgets((char*)i.m + i.r(0), (int)i.r(1), f) ? i.r(0) : 0); });
    add("fgetc", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret(f ? (uint32_t)std::fgetc(f) : 0xFFFFFFFFu); });
    add("getc", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret(f ? (uint32_t)std::fgetc(f) : 0xFFFFFFFFu); });
    add("ungetc", [](I& i) { FILE* f = i.file_of(i.r(1)); i.ret(f ? (uint32_t)std::ungetc((int)i.r(0), f) : 0xFFFFFFFFu); });
    add("fputc", [out_file](I& i) { std::fputc((int)i.r(0), out_file(i, i.r(1))); i.ret(i.r(0)); });
    add("putc", [out_file](I& i) { std::fputc((int)i.r(0), out_file(i, i.r(1))); i.ret(i.r(0)); });
    add("fputs", [out_file](I& i) { std::fputs(i.str(i.r(0)), out_file(i, i.r(1))); i.ret(1); });
    add("feof", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret(f ? (uint32_t)std::feof(f) : 1); });
    add("ferror", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret(f ? (uint32_t)std::ferror(f) : 1); });
    add("rewind", [](I& i) { if (FILE* f = i.file_of(i.r(0))) std::rewind(f); });
    add("fseek", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret(f ? (uint32_t)std::fseek(f, (int32_t)i.r(1), (int)i.r(2)) : 0xFFFFFFFFu); });
    add("ftell", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret(f ? (uint32_t)std::ftell(f) : 0xFFFFFFFFu); });
    add("fseeko64", [](I& i) { FILE* f = i.file_of(i.r(0)); const int64_t off = (int64_t)((uint64_t)i.r(2) | (uint64_t)i.r(3) << 32); i.ret(f ? (uint32_t)fseeko(f, off, (int)i.arg(4)) : 0xFFFFFFFFu); });
    add("ftello64", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret64(f ? (uint64_t)ftello(f) : ~0ull); });
    add("fprintf", [](I& i) { const std::string s = i.format(i.str(i.r(1)), {i, 2, i.r(13)}); if (FILE* f = i.file_of(i.r(0))) std::fputs(s.c_str(), f); else std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    add("__fprintf_chk", [](I& i) { const std::string s = i.format(i.str(i.r(2)), {i, 3, i.r(13)}); if (FILE* f = i.file_of(i.r(0))) std::fputs(s.c_str(), f); else std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    auto open_ = [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(2)); return; }
        const uint32_t a = i.r(1);   // ARM O_* differ from x86 for DIRECTORY, NOFOLLOW, DIRECT and LARGEFILE
        int fl = (int)(a & 0x3 ) | (int)(a & (0100 | 0200 | 0400 | 01000 | 02000 | 04000 | 02000000));
        if (a & 040000) fl |= O_DIRECTORY;
        if (a & 0100000) fl |= O_NOFOLLOW;
        const int fd = ::open(hp.c_str(), fl, (mode_t)i.r(2));
        i.ret(fd < 0 ? i.fail(errno) : (uint32_t)fd); };
    add("open", open_); add("open64", open_);
    add("__open64_2", open_); add("__openat64_2", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(1)), hp)) { i.ret(i.fail(2)); return; } const int fd = ::open(hp.c_str(), (int)i.r(2) & 3); i.ret(fd < 0 ? i.fail(errno) : (uint32_t)fd); });
    add("close", [](I& i) { i.ret(i.r(0) > 2 ? (uint32_t)::close((int)i.r(0)) : 0); });
    add("read", [](I& i) { const ssize_t n = ::read((int)i.r(0), i.m + i.r(1), i.r(2)); i.ret(n < 0 ? i.fail(errno) : (uint32_t)n); });
    add("__read_chk", [](I& i) { const ssize_t n = ::read((int)i.r(0), i.m + i.r(1), i.r(2)); i.ret(n < 0 ? i.fail(errno) : (uint32_t)n); });
    add("write", [](I& i) { const int fd = (int)i.r(0) == 1 ? 2 : (int)i.r(0); const ssize_t n = ::write(fd, i.m + i.r(1), i.r(2)); i.ret(n < 0 ? i.fail(errno) : (uint32_t)n); });
    add("lseek64", [](I& i) { const int64_t off = (int64_t)((uint64_t)i.r(2) | (uint64_t)i.r(3) << 32); i.ret64((uint64_t)::lseek((int)i.r(0), off, (int)i.arg(4))); });
    add("access", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(2)); return; } i.ret(::access(hp.c_str(), (int)i.r(1)) ? i.fail(errno) : 0); });
    add("mkdir", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(13)); return; } i.ret(::mkdir(hp.c_str(), (mode_t)i.r(1)) ? i.fail(errno) : 0); });
    add("remove", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(13)); return; } i.ret(std::remove(hp.c_str()) ? i.fail(errno) : 0); });
    add("rename", [](I& i) { std::string a, b; if (!i.hostpath(i.str(i.r(0)), a) || !i.hostpath(i.str(i.r(1)), b)) { i.ret(i.fail(13)); return; } i.ret(std::rename(a.c_str(), b.c_str()) ? i.fail(errno) : 0); });
    add("getcwd", [](I& i) { if (i.r(1) < 2) { i.ret(0); return; } std::memcpy(i.m + i.r(0), "/", 2); i.ret(i.r(0)); });
    // armhf `struct stat` (88 bytes, 32-bit st_size at 44) for __xstat/__lxstat/__fxstat, and `struct stat64` (st_size 64-bit at 48) for the *64 forms
    auto put_stat = [](I& i, gaddr a, const struct stat& st, int mode) {   // mode 0: stat, 1: stat64, 2: stat with 64-bit times (glibc 2.34+, 112 bytes)
        std::memset(i.m + a, 0, mode == 0 ? 88 : mode == 1 ? 104 : 112);   // exactly the guest struct: it sits on the guest stack
        i.wr<uint32_t>(a + 16, st.st_mode); i.wr<uint32_t>(a + 20, (uint32_t)st.st_nlink); i.wr<uint32_t>(a + 24, st.st_uid); i.wr<uint32_t>(a + 28, st.st_gid);
        i.wr<uint64_t>(a, st.st_dev); i.wr<uint64_t>(a + 32, st.st_rdev);
        if (mode == 2) {
            i.wr<uint64_t>(a + 8, st.st_ino); i.wr<uint64_t>(a + 40, (uint64_t)st.st_size); i.wr<uint32_t>(a + 48, (uint32_t)st.st_blksize); i.wr<uint64_t>(a + 56, (uint64_t)st.st_blocks);
            i.wr<int64_t>(a + 64, st.st_atime); i.wr<int64_t>(a + 80, st.st_mtime); i.wr<int64_t>(a + 96, st.st_ctime);
        } else if (mode == 1) {
            i.wr<uint32_t>(a + 12, (uint32_t)st.st_ino); i.wr<uint64_t>(a + 48, (uint64_t)st.st_size); i.wr<uint32_t>(a + 56, (uint32_t)st.st_blksize); i.wr<uint64_t>(a + 64, (uint64_t)st.st_blocks);
            i.wr<uint32_t>(a + 72, (uint32_t)st.st_atime); i.wr<uint32_t>(a + 80, (uint32_t)st.st_mtime); i.wr<uint32_t>(a + 88, (uint32_t)st.st_ctime); i.wr<uint64_t>(a + 96, st.st_ino);
        } else {
            i.wr<uint32_t>(a + 12, (uint32_t)st.st_ino); i.wr<uint32_t>(a + 44, (uint32_t)st.st_size); i.wr<uint32_t>(a + 48, (uint32_t)st.st_blksize); i.wr<uint32_t>(a + 52, (uint32_t)st.st_blocks);
            i.wr<uint32_t>(a + 56, (uint32_t)st.st_atime); i.wr<uint32_t>(a + 64, (uint32_t)st.st_mtime); i.wr<uint32_t>(a + 72, (uint32_t)st.st_ctime); i.wr<uint64_t>(a + 80, st.st_ino);
        }
    };
    for (const char* n : {"__xstat", "__xstat64", "__lxstat", "__lxstat64"}) {   // (ver, path, buf)
        const int mode = std::strstr(n, "64") != nullptr ? 1 : 0; const bool lst = std::strstr(n, "lx") != nullptr;
        add(n, [mode, lst, put_stat](I& i) {
            std::string hp; if (!i.hostpath(i.str(i.r(1)), hp)) { i.ret(i.fail(2)); return; }
            struct stat st; if ((lst ? ::lstat(hp.c_str(), &st) : ::stat(hp.c_str(), &st)) != 0) { i.ret(i.fail(errno)); return; }
            put_stat(i, i.r(2), st, mode); i.ret(0); });
    }
    for (const char* n : {"__stat64_time64", "__lstat64_time64"}) {   // (path, buf)
        const bool lst = std::strstr(n, "lstat") != nullptr;
        add(n, [lst, put_stat](I& i) {
            std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(2)); return; }
            struct stat st; if ((lst ? ::lstat(hp.c_str(), &st) : ::stat(hp.c_str(), &st)) != 0) { i.ret(i.fail(errno)); return; }
            put_stat(i, i.r(1), st, 2); i.ret(0); });
    }
    for (const char* n : {"__fxstat", "__fxstat64"}) {
        const int mode = std::strstr(n, "64") != nullptr ? 1 : 0;
        add(n, [mode, put_stat](I& i) { struct stat st; if (::fstat((int)i.r(1), &st) != 0) { i.ret(i.fail(errno)); return; } put_stat(i, i.r(2), st, mode); i.ret(0); });
    }
    add("__fstat64_time64", [put_stat](I& i) { struct stat st; if (::fstat((int)i.r(0), &st) != 0) { i.ret(i.fail(errno)); return; } put_stat(i, i.r(1), st, 2); i.ret(0); });
    // fscanf: read ahead into a buffer, scan it, and seek back over what was not consumed (regular files)
    auto fscanf_ = [](I& i) {
        FILE* f = i.file_of(i.r(0)); if (!f) { i.ret(0xFFFFFFFFu); return; }
        const long pos = std::ftell(f); char buf[4096]; const size_t got = std::fread(buf, 1, sizeof buf - 1, f); buf[got] = 0;
        const int n = i.scan(buf, i.str(i.r(1)), {i, 2, i.r(13)});
        if (pos >= 0) std::fseek(f, pos + (long)i.scan_used, SEEK_SET);
        i.ret((uint32_t)n); };
    for (const char* n : {"fscanf", "__isoc99_fscanf", "__isoc23_fscanf"}) add(n, fscanf_);
    // directories: DIR* is a guest handle; the dirent lives in a 288-byte guest buffer (armhf dirent: d_ino +0, d_type +10, d_name +11; dirent64: d_ino +0, d_type +18, d_name +19)
    add("opendir", [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.set_errno(2); i.ret(0); return; }
        DIR* d = ::opendir(hp.c_str()); if (!d) { i.set_errno(errno); i.ret(0); return; }
        const gaddr h = i.malloc_(288); i.dirs[h] = d; i.ret(h); });
    auto readdir_ = [](I& i, bool is64) {
        auto it = i.dirs.find(i.r(0)); if (it == i.dirs.end()) { i.ret(0); return; }
        const dirent* e = ::readdir(it->second); if (!e) { i.ret(0); return; }
        const gaddr b = i.r(0); std::memset(i.m + b, 0, 288);
        if (is64) { i.wr<uint64_t>(b, e->d_ino); i.wr<uint16_t>(b + 16, 280); i.m[b + 18] = e->d_type; std::strncpy((char*)i.m + b + 19, e->d_name, 255); }
        else { i.wr<uint32_t>(b, (uint32_t)e->d_ino); i.wr<uint16_t>(b + 8, 280); i.m[b + 10] = e->d_type; std::strncpy((char*)i.m + b + 11, e->d_name, 255); }
        i.ret(b); };
    add("readdir", [readdir_](I& i) { readdir_(i, false); });
    add("readdir64", [readdir_](I& i) { readdir_(i, true); });
    add("closedir", [](I& i) { auto it = i.dirs.find(i.r(0)); if (it != i.dirs.end()) { ::closedir(it->second); i.dirs.erase(it); i.free_(i.r(0)); } i.ret(0); });
    // glob_t (armhf): gl_pathc +0, gl_pathv +4, gl_offs +8
    auto glob_ = [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(3); return; }
        glob_t g{}; if (::glob(hp.c_str(), 0, nullptr, &g) != 0) { i.ret(3); return; }   // GLOB_NOMATCH
        const gaddr vec = i.malloc_(4 * ((uint32_t)g.gl_pathc + 1));
        for (size_t k = 0; k < g.gl_pathc; k++) i.wr<uint32_t>(vec + 4 * (uint32_t)k, i.g.strdup(i.guestpath(g.gl_pathv[k]).c_str()));
        i.wr<uint32_t>(vec + 4 * (uint32_t)g.gl_pathc, 0);
        i.wr<uint32_t>(i.r(3), (uint32_t)g.gl_pathc); i.wr<uint32_t>(i.r(3) + 4, vec); i.wr<uint32_t>(i.r(3) + 8, 0);
        globfree(&g); i.ret(0); };
    for (const char* n : {"glob", "glob64", "__glob64_time64"}) add(n, glob_);
    for (const char* n : {"globfree", "globfree64", "__globfree64_time64"}) add(n, [](I& i) { i.ret(0); });   // the vectors are left in the guest heap
    // dladdr: Dl_info { fname, fbase, sname, saddr }; /plugin/<name> for the loaded plugin, /lib/<name> for libraries
    add("dladdr", [](I& i) {
        for (size_t k = 0; k < i.mods.size(); k++) {
            if (i.r(0) < i.mods[k].base || i.r(0) >= i.mods[k].end) continue;
            const std::string nm = std::filesystem::path(i.mod_paths[k]).filename().string();
            const bool is_lib = k + 1 < i.mods.size() && std::getenv("MPC_HOST_LIBS");   // libraries load before the plugin
            i.wr<uint32_t>(i.r(1), i.g.strdup(((is_lib ? "/lib/" : "/plugin/") + nm).c_str())); i.wr<uint32_t>(i.r(1) + 4, i.mods[k].base); i.wr<uint32_t>(i.r(1) + 8, 0); i.wr<uint32_t>(i.r(1) + 12, 0);
            i.ret(1); return;
        }
        i.ret(0); });
    for (const char* n : {"getpwnam", "getpwuid", "getenv_s"}) add(n, [](I& i) { i.ret(0); });   // no user database
    add("__aeabi_read_tp", [](I& i) { i.ret(kTp); });
    add("__tls_get_addr", [](I& i) { const gaddr ti = i.r(0); i.ret(i.tls.at(i.rd<uint32_t>(ti) - 1).block + i.rd<uint32_t>(ti + 4)); });
    // C++ runtime
    for (const char* n : {"_Znwj", "_Znaj", "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t"})
        add(n, [](I& i) { i.ret(i.malloc_(i.r(0))); });
    for (const char* n : {"_ZdlPv", "_ZdaPv", "_ZdlPvj", "_ZdaPvj", "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t"})
        add(n, [](I& i) { i.free_(i.r(0)); });
    add("__cxa_guard_acquire", [](I& i) { i.ret(i.m[i.r(0)] ? 0 : 1); });
    add("__cxa_guard_release", [](I& i) { i.m[i.r(0)] = 1; });
    add("__cxa_guard_abort", [](I&) {});
    add("__cxa_pure_virtual", [](I& i) { abort_named(i, "__cxa_pure_virtual"); });
    // libm (double in d0/d1, float in s0/s1)
#define D1(n) add(#n, [](I& i) { i.ret_d(std::n(i.d(0))); });
#define D2(n) add(#n, [](I& i) { i.ret_d(std::n(i.d(0), i.d(1))); });
#define F1(n, h) add(#n, [](I& i) { i.ret_f(std::h(i.s(0))); });
#define F2(n, h) add(#n, [](I& i) { i.ret_f(std::h(i.s(0), i.s(1))); });
    D1(exp) D1(exp2) D1(log) D1(log2) D1(log10) D1(log1p) D1(expm1) D1(sin) D1(cos) D1(tan) D1(sinh) D1(cosh) D1(tanh) D1(asin) D1(acos) D1(atan)
    D1(sqrt) D1(floor) D1(ceil) D1(round) D1(trunc) D1(fabs) D1(cbrt)
    D2(pow) D2(atan2) D2(fmod) D2(fmax) D2(fmin) D2(hypot)
    F1(expf, exp) F1(exp2f, exp2) F1(logf, log) F1(log2f, log2) F1(log10f, log10) F1(sinf, sin) F1(cosf, cos) F1(tanf, tan) F1(tanhf, tanh)
    F1(sqrtf, sqrt) F1(floorf, floor) F1(ceilf, ceil) F1(roundf, round) F1(truncf, trunc) F1(fabsf, fabs) F1(atanf, atan)
    F2(powf, pow) F2(atan2f, atan2) F2(fmodf, fmod) F2(fmaxf, fmax) F2(fminf, fmin)
    add("lrintf", [](I& i) { i.ret((uint32_t)(int32_t)std::lrint(i.s(0))); });
    add("lroundf", [](I& i) { i.ret((uint32_t)(int32_t)std::lround(i.s(0))); });
    add("lrint", [](I& i) { i.ret((uint32_t)(int32_t)std::lrint(i.d(0))); });
    add("lround", [](I& i) { i.ret((uint32_t)(int32_t)std::lround(i.d(0))); });
    add("sincosf", [](I& i) { const float x = i.s(0); i.wr<float>(i.r(0), std::sin(x)); i.wr<float>(i.r(1), std::cos(x)); });
    add("sincos", [](I& i) { const double x = i.d(0); i.wr<double>(i.r(0), std::sin(x)); i.wr<double>(i.r(1), std::cos(x)); });
#undef D1
#undef D2
#undef F1
#undef F2
    return t;
}

// Imports that are data, not code.
bool is_data_import(const std::string& n) { return n == "__stack_chk_guard" || n == "_ZSt7nothrow" || n == "stderr" || n == "stdout"; }
}  // namespace

Guest::Guest() {
    void* p = mmap(nullptr, kSpace, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap 4 GB"); std::abort(); }
    mem_ = static_cast<uint8_t*>(p);
    impl_ = new Impl(*this);
    // index 0: a trap at guest address 0 (a call through a null pointer, e.g. an unresolved weak import)
    impl_->add_thunk("<null>", [](I& i) { std::fprintf(stderr, "guest: call to address 0 from lr=%08x\n", i.r(14)); std::abort(); });
    impl_->wr<uint32_t>(0, 0xEF000000u);   // svc #0 at address 0
    impl_->th.push_back(std::make_unique<Guest::Impl::GThread>());
    impl_->th[0]->id = 1;
    impl_->thread_exit_stub = impl_->add_thunk("<thread exit>", [](I& i) { i.exit_thread(i.r(0)); });
    const gaddr ret = kData - 8;
    impl_->wr<uint32_t>(ret, 0xEF000000u | kRetSvc);

    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = impl_;
    cfg.global_monitor = &impl_->monitor;
    cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(mem_);
    cfg.arch_version = Dynarmic::A32::ArchVersion::v7;   // the plugins are built for armv7-a
    cfg.enable_cycle_counting = false;
    if (std::getenv("MPC_HOST_UNSAFE")) {   // faster float maths, less exact NaN/rounding handling (dynarmic's "unsafe" flags)
        cfg.unsafe_optimizations = true;
        cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                             Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN | Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    }
    impl_->jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
}

Guest::~Guest() {
    delete impl_;
    munmap(mem_, kSpace);
}

uint32_t Guest::rd32(gaddr a) const { return impl_->rd<uint32_t>(a); }
void Guest::wr32(gaddr a, uint32_t v) { impl_->wr<uint32_t>(a, v); }
gaddr Guest::alloc(uint32_t n) { return impl_->malloc_(n); }
void Guest::dealloc(gaddr p) { impl_->free_(p); }
gaddr Guest::strdup(const char* s) { const uint32_t n = (uint32_t)std::strlen(s) + 1; const gaddr p = alloc(n); std::memcpy(mem_ + p, s, n); return p; }

gaddr Guest::symbol(const char* name) const {
    auto it = impl_->dynsyms.find(name);
    return it == impl_->dynsyms.end() ? 0 : it->second;
}

gaddr Guest::host_callback(HostCallback fn) {
    return impl_->add_thunk("<host callback>", [fn](I& i) {
        float opt; std::memcpy(&opt, &i.jit->ExtRegs()[0], 4);
        i.ret(fn(i.g, i.r(0), (int32_t)i.r(1), (int32_t)i.r(2), (int32_t)i.r(3), i.arg(4), opt));
    });
}

uint32_t Guest::call(gaddr fn, const std::vector<uint32_t>& ints, const std::vector<float>& floats) {
    I& i = *impl_;
    if (i.depth) { std::fprintf(stderr, "guest: nested call from a thunk (not supported)\n"); std::abort(); }
    auto& R = i.jit->Regs();
    const auto saved = R;
    uint32_t sp = R[13] ? R[13] : kStackTop;
    const size_t nstack = ints.size() > 4 ? ints.size() - 4 : 0;
    sp -= (uint32_t)(4 * nstack);
    sp &= ~7u;
    for (size_t k = 0; k < nstack; k++) i.wr<uint32_t>(sp + 4 * (uint32_t)k, ints[4 + k]);
    for (size_t k = 0; k < 4 && k < ints.size(); k++) R[k] = ints[k];
    for (size_t k = 0; k < floats.size() && k < 16; k++) std::memcpy(&i.jit->ExtRegs()[k], &floats[k], 4);
    R[13] = sp;
    R[14] = kData - 8;   // return stub
    R[15] = fn & ~1u;
    i.jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));   // user mode, Thumb if bit 0
    i.call_no++; i.cur = 0; i.main_active = true;
    i.depth++;
    i.jit->Run();
    i.jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
    i.depth--;
    i.main_active = false;
    const uint32_t r0 = R[0];
    const uint32_t s0 = i.jit->ExtRegs()[0], s1 = i.jit->ExtRegs()[1];   // a double result lives in s0/s1
    i.drain();
    i.jit->ExtRegs()[0] = s0; i.jit->ExtRegs()[1] = s1;
    R = saved;
    R[0] = r0;
    if (!saved[13]) R[13] = 0;
    return r0;
}

float Guest::call_f(gaddr fn, const std::vector<uint32_t>& ints, const std::vector<float>& floats) {
    call(fn, ints, floats);
    return impl_->s(0);
}

bool Guest::load(const std::string& path) {
    I& i = *impl_;
    const gaddr bias = i.load_next;
    std::ifstream f(path, std::ios::binary);
    if (!f) { err_ = "cannot open " + path; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    if (file.size() < sizeof(Elf32_Ehdr)) { err_ = "not an ELF file"; return false; }
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(file.data());
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM || eh->e_type != ET_DYN) {
        err_ = "not a 32-bit ARM shared object (a Gen2 aarch64 build needs the A64 front end)"; return false;
    }
    const auto* ph = reinterpret_cast<const Elf32_Phdr*>(file.data() + eh->e_phoff);
    const Elf32_Dyn* dyn = nullptr;
    uint32_t end = 0, tls_id = 0;
    std::pair<gaddr, uint32_t> exidx{0, 0};
    for (int k = 0; k < eh->e_phnum; k++) {
        if (ph[k].p_type == PT_LOAD) end = std::max(end, ph[k].p_vaddr + ph[k].p_memsz);
        if (ph[k].p_type == PT_ARM_EXIDX) exidx = {bias + ph[k].p_vaddr, ph[k].p_memsz};
        if (ph[k].p_type == PT_LOAD) {
            std::memcpy(mem_ + bias + ph[k].p_vaddr, file.data() + ph[k].p_offset, ph[k].p_filesz);
            std::memset(mem_ + bias + ph[k].p_vaddr + ph[k].p_filesz, 0, ph[k].p_memsz - ph[k].p_filesz);
        } else if (ph[k].p_type == PT_TLS) {
            // single-threaded: one block per module, at tp + 8 + offset (ARM TLS variant 1)
            const uint32_t al = std::max<uint32_t>(ph[k].p_align, 1);
            i.tls_used = (i.tls_used + al - 1) / al * al;
            const gaddr blk = kTp + 8 + i.tls_used;
            if (blk + ph[k].p_memsz > kLoad) { err_ = "TLS area full"; return false; }
            std::memset(mem_ + blk, 0, ph[k].p_memsz);
            std::memcpy(mem_ + blk, file.data() + ph[k].p_offset, ph[k].p_filesz);
            i.tls.push_back({blk, i.tls_used});
            i.tls_used += ph[k].p_memsz;
            tls_id = (uint32_t)i.tls.size();
        } else if (ph[k].p_type == PT_DYNAMIC) dyn = reinterpret_cast<const Elf32_Dyn*>(mem_ + bias + ph[k].p_vaddr);
    }
    if (!dyn) { err_ = "no PT_DYNAMIC"; return false; }
    i.load_next = (bias + end + 0xFFFFu) & ~0xFFFFu;
    i.mods.push_back({bias, bias + end, exidx.first, exidx.second});
    i.mod_paths.push_back(path);
    if (std::getenv("MPC_HOST_TRACE")) std::fprintf(stderr, "module %s at %08x-%08x\n", path.c_str(), bias, bias + end);
    i.plugin_dir = std::filesystem::path(path).parent_path().string();
    // dynamic section (read after the copy: it lives in a loaded segment)
    uint32_t symtab = 0, strtab = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, init = 0, init_array = 0, init_arraysz = 0;
    for (const Elf32_Dyn* d = reinterpret_cast<const Elf32_Dyn*>(mem_ + (reinterpret_cast<const uint8_t*>(dyn) - mem_)); d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
        case DT_STRTAB: strtab = d->d_un.d_ptr; break;
        case DT_REL: rel = d->d_un.d_ptr; break;
        case DT_RELSZ: relsz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_INIT: init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_un.d_val; break;
        case DT_RELA: err_ = "RELA relocations (not armhf)"; return false;
        }
    }
    // symbol count from the section headers (.dynsym)
    uint32_t nsyms = 0;
    const auto* sh = reinterpret_cast<const Elf32_Shdr*>(file.data() + eh->e_shoff);
    for (int k = 0; k < eh->e_shnum; k++) if (sh[k].sh_type == SHT_DYNSYM) nsyms = sh[k].sh_size / sizeof(Elf32_Sym);
    const auto* syms = reinterpret_cast<const Elf32_Sym*>(mem_ + bias + symtab);
    const char* strs = reinterpret_cast<const char*>(mem_ + bias + strtab);

    // imports -> thunks
    std::unordered_map<std::string, Thunk> table;
    for (auto& e : libc_table()) table.emplace(e.name, std::move(e.fn));
    std::vector<gaddr> symaddr(nsyms, 0);
    const size_t missing_before = missing_.size();
    for (uint32_t k = 1; k < nsyms; k++) {
        const std::string name = strs + syms[k].st_name;
        if (syms[k].st_shndx != SHN_UNDEF) {
            symaddr[k] = bias + syms[k].st_value;
            if (ELF32_ST_TYPE(syms[k].st_info) == STT_TLS) symaddr[k] = syms[k].st_value;   // an offset in this module's TLS block
            if (ELF32_ST_BIND(syms[k].st_info) != STB_LOCAL && ELF32_ST_TYPE(syms[k].st_info) != STT_TLS) i.dynsyms.emplace(name, symaddr[k]);
            continue;
        }
        if (name.empty()) continue;
        if (auto d = i.dynsyms.find(name); d != i.dynsyms.end()) { symaddr[k] = d->second; continue; }   // a library loaded before this one
        if (is_data_import(name)) {
            auto& da = i.data_by_name[name];
            if (da) { symaddr[k] = da; continue; }
            const gaddr a = da = i.data(64);
            if (name == "__stack_chk_guard") i.wr<uint32_t>(a, 0xC0FFEE00u);
            symaddr[k] = a;
            continue;
        }
        if (name == "__ctype_b_loc" && !i.thunk_by_name.count(name)) {
            // glibc's table is indexed from -128; both armhf and x86-64 glibc are little-endian with the same bits
            const gaddr tbl = i.data(384 * 2, 4), pp = i.data(4, 4);
            std::memcpy(mem_ + tbl, *__ctype_b_loc() - 128, 384 * 2);
            i.wr<uint32_t>(pp, tbl + 256);
            symaddr[k] = i.thunk_for(name, [pp](I& ii) { ii.ret(pp); });
            continue;
        }
        if (name == "pthread_once") {   // guest code, so the init routine runs without a nested call: if (!*once) { *once = 1; tail-call init }
            symaddr[k] = i.thunk_by_name.count(name) ? i.thunk_by_name[name] : (i.thunk_by_name[name] = i.stub({0xE5902000, 0xE3520000, 0x112FFF1E, 0xE3A02001, 0xE5802000, 0xE12FFF11}));
            continue;
        }
        auto it = table.find(name);
        if (it != table.end()) { symaddr[k] = i.thunk_for(name, it->second); continue; }
        if (ELF32_ST_BIND(syms[k].st_info) == STB_WEAK) { symaddr[k] = 0; continue; }
        missing_.push_back(name);
        symaddr[k] = i.thunk_for(name, [name](I& ii) { abort_named(ii, name); });
    }

    for (size_t k = missing_before; k < missing_.size(); k++) std::fprintf(stderr, "  no thunk for import: %s\n", missing_[k].c_str());
    // relocations
    auto apply = [&](uint32_t at, uint32_t size) -> bool {
        for (uint32_t off = 0; off < size; off += sizeof(Elf32_Rel)) {
            const auto* rr = reinterpret_cast<const Elf32_Rel*>(mem_ + bias + at + off);
            const uint32_t type = ELF32_R_TYPE(rr->r_info), sym = ELF32_R_SYM(rr->r_info);
            const gaddr P = bias + rr->r_offset;
            const uint32_t S = sym < nsyms ? symaddr[sym] : 0;
            switch (type) {
            case R_ARM_RELATIVE: i.wr<uint32_t>(P, i.rd<uint32_t>(P) + bias); break;
            case R_ARM_GLOB_DAT: case R_ARM_JUMP_SLOT: i.wr<uint32_t>(P, S); break;
            case R_ARM_ABS32: i.wr<uint32_t>(P, i.rd<uint32_t>(P) + S); break;
            case R_ARM_NONE: break;
            case R_ARM_TLS_DTPMOD32: i.wr<uint32_t>(P, tls_id); break;                                   // this module (symbols are local to it)
            case R_ARM_TLS_DTPOFF32: i.wr<uint32_t>(P, i.rd<uint32_t>(P) + S); break;
            case R_ARM_TLS_TPOFF32: i.wr<uint32_t>(P, i.rd<uint32_t>(P) + S + 8 + (tls_id ? i.tls[tls_id - 1].off : 0)); break;
            default: err_ = "unsupported relocation type " + std::to_string(type); return false;
            }
        }
        return true;
    };
    if (!apply(rel, relsz) || !apply(jmprel, pltrelsz)) return false;

    // constructors
    if (init) call(bias + init, {});
    for (uint32_t k = 0; k < init_arraysz / 4; k++) {
        const gaddr fn = i.rd<uint32_t>(bias + init_array + 4 * k);
        if (fn && fn != 0xffffffffu) call(fn, {});
    }
    return true;
}

}  // namespace mpchost

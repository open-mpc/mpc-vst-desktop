// Prototype guest: 4 GB guest space, a small ELF32 loader for armhf shared objects, dynarmic A32, and thunks that
// route the plugin's libc/libm/libstdc++ imports to host code. See docs/MPC_HOST.md.
#include "guest.h"
#include "hostfs.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
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
#ifdef _WIN32
#include <windows.h>
#include "elf_defs.h"
#else
#include <unistd.h>
#include <elf.h>
#include <sys/mman.h>
#endif
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>
#include <cwctype>
#include <chrono>

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
constexpr uint32_t kRetSvc = 0xFFFFFF;    // svc number of the "return to host" stub (A64: svc takes 16 bits, so kRetSvc64)
constexpr uint32_t kRetSvc64 = 0xFFFF;
constexpr gaddr kHeapEnd64 = 0x70000000;  // aarch64 guests keep every address below 2 GB: a 32-bit value sign-extended to 64 bits is still the same pointer
constexpr gaddr kStackTop64 = 0x7FF00000;
}  // namespace

struct Impl;
using Thunk = std::function<void(Guest::Impl&)>;

struct Guest::Impl final : Dynarmic::A32::UserCallbacks, Dynarmic::A64::UserCallbacks {
    Guest& g;
    uint8_t* m;
    bool a64 = false;                        // aarch64 guest (jit64) instead of armhf (jit)
    Dynarmic::ExclusiveMonitor monitor{1};   // LDREX/STREX (one guest CPU)
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::unique_ptr<Dynarmic::A64::Jit> jit64;
    uint64_t tpidr_el0 = 0;                  // the aarch64 thread pointer (TPIDR_EL0) is read from here
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
    struct Mod { gaddr base, end, exidx; uint32_t exidx_size; gaddr eh = 0; };   // eh: .eh_frame_hdr (PT_GNU_EH_FRAME) for _dl_find_object   // for __gnu_Unwind_Find_exidx (ARM EHABI unwinding)
    std::vector<Mod> mods;
    // dl_iterate_phdr (older libgcc finds exception tables with it): the callback is guest code, so the loop is guest code too (assembled with the cross assemblers
    // from a loop that calls cb(info, sizeof info, data) for each module until it returns non-zero); the host keeps the dl_phdr_info records it walks.
    gaddr dlphdr_region = 0, dlphdr_stub = 0;   // region: u32 count at +0, records from +8 (64 bytes each on aarch64, 40 on armhf)
    void register_phdr(gaddr bias, const std::string& path, const void* phdrs, int phnum, size_t entsz) {
        const uint32_t rec = a64 ? 64 : 40;
        if (!dlphdr_region) { dlphdr_region = data(8 + 32 * 64, 8); wr<uint32_t>(dlphdr_region, 0); }
        const uint32_t n = rd<uint32_t>(dlphdr_region);
        if (n >= 32) return;
        const gaddr copy = data((uint32_t)(entsz * phnum), 8); std::memcpy(m + copy, phdrs, entsz * phnum);   // the table is copied: the file's headers need not be mapped
        const gaddr r = dlphdr_region + 8 + n * rec; std::memset(m + r, 0, rec);
        wrp(r, bias); wrp(r + (a64 ? 8 : 4), cstr(path.c_str())); wrp(r + (a64 ? 16 : 8), copy); wr<uint16_t>(r + (a64 ? 24 : 12), (uint16_t)phnum);
        wr<uint32_t>(dlphdr_region, n + 1);
    }
    gaddr dl_iterate_phdr_stub() {
        if (dlphdr_stub) return dlphdr_stub;
        if (!dlphdr_region) { dlphdr_region = data(8 + 32 * 64, 8); wr<uint32_t>(dlphdr_region, 0); }
        static const std::vector<uint32_t> A64 = {0xA9BD7BFD, 0x910003FD, 0xA90153F3, 0xA9025BF5, 0xAA0003F3, 0xAA0103F4, 0x10000249, 0xF9400135, 0xB94002B6, 0x910022B5, 0x52800000, 0x34000136,
                                                  0xAA1503E0, 0xD2800801, 0xAA1403E2, 0xD63F0260, 0x35000080, 0x910102B5, 0x510006D6, 0x17FFFFF8, 0xA9425BF5, 0xA94153F3, 0xA8C37BFD, 0xD65F03C0, 0, 0};
        static const std::vector<uint32_t> A32 = {0xE92D40F0, 0xE1A04000, 0xE1A05001, 0xE59F6038, 0xE5967000, 0xE2866008, 0xE3A00000, 0xE3570000, 0x0A000008, 0xE1A00006, 0xE3A01028,
                                                  0xE1A02005, 0xE12FFF34, 0xE3500000, 0x1A000002, 0xE2866028, 0xE2477001, 0xEAFFFFF4, 0xE8BD80F0, 0};
        std::vector<uint32_t> code = a64 ? A64 : A32;
        code[a64 ? 24 : 19] = dlphdr_region;   // the literal after the code: where the records are (aarch64: the low half of a .quad)
        return dlphdr_stub = stub(code);
    }
    std::vector<std::string> stubs_called;   // unsupported imports the guest actually called (MPC_HOST_LAX only; otherwise the first one aborts)
    gaddr errno_p = 0, clocale = 0;
    // --- files: guest paths are mapped into MPC_HOST_ROOT (so /sdcard/x is $MPC_HOST_ROOT/sdcard/x); /plugin/ is the plugin's own directory ---
    std::map<int, std::pair<std::vector<uint8_t>, size_t>> http;   // the lent HTTP GETs: body and read position
    int next_http = 1, next_pid = 2000;
    std::map<int, int> exits;   // pid -> exit status of the lent spawns
    std::unordered_map<gaddr, FILE*> files;
    std::unordered_map<gaddr, hostfs::Dir> dirs;
    std::string plugin_dir;
    std::vector<std::string> mod_paths;   // parallel to mods
    // host paths are kept with '/' separators (Windows accepts them) and no trailing slash, so guest and host paths map both ways whatever the host's own style
    static std::string slashes(std::string p) { for (char& c : p) if (c == '\\') c = '/'; while (p.size() > 1 && p.back() == '/') p.pop_back(); return p; }
    static std::string root_dir() { const char* r = std::getenv("MPC_HOST_ROOT"); return r ? slashes(r) : std::string(); }
    bool hostpath(const char* gp, std::string& out) {
        std::string p = gp ? gp : "";
        if (p.empty()) return false;
        if (p[0] != '/') p = "/" + p;
        if (p.find("/../") != std::string::npos || (p.size() > 3 && p.compare(p.size() - 3, 3, "/..") == 0)) return false;
        if (p == "/plugin" || p.rfind("/plugin/", 0) == 0) { out = plugin_dir + p.substr(7); return true; }
        if (const std::string r = root_dir(); !r.empty()) { out = r + p; return true; }
        return false;
    }
    std::string guestpath(const std::string& hp) {
        if (!plugin_dir.empty() && hp.rfind(plugin_dir, 0) == 0) return "/plugin" + hp.substr(plugin_dir.size());
        if (const std::string rr = root_dir(); !rr.empty() && hp.rfind(rr, 0) == 0) return hp.substr(rr.size());
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

    Impl(Guest& g_, bool a64_) : a64(a64_), g(g_), m(g_.mem_) {}
    // Windows reserves the 4 GB guest arena and commits it in 1 MB steps as the guest uses it (the low data area, module segments, heap growth, stacks); mmap is already lazy elsewhere.
    std::vector<uint8_t> committed = std::vector<uint8_t>(4096, 0);
    void commit(uint32_t lo, uint32_t hi) {
#ifdef _WIN32
        for (uint32_t c = lo >> 20; c <= (hi - 1) >> 20 && c < 4096; c++) {
            if (committed[c]) continue;
            if (!VirtualAlloc(m + ((uint64_t)c << 20), 1u << 20, MEM_COMMIT, PAGE_READWRITE)) { std::fprintf(stderr, "guest: cannot commit memory (error %lu)\n", GetLastError()); std::abort(); }
            committed[c] = 1;
        }
#else
        (void)lo; (void)hi;
#endif
    }

    // --- dynarmic callbacks (fastmem covers almost everything; these are the slow path) ---
    template <class T> T rd(uint32_t a) { T v; std::memcpy(&v, m + a, sizeof v); return v; }
    // a pointer-sized value in guest memory (4 bytes armhf, 8 bytes aarch64; guest addresses are below 2 GB)
    uint32_t rdp(uint32_t a) { return rd<uint32_t>(a); }
    void wrp(uint32_t a, uint32_t v) { wr<uint32_t>(a, v); if (a64) wr<uint32_t>(a + 4, 0); }
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
    // aarch64 (the same slow paths with 64-bit addresses; every guest address is below 4 GB)
    uint8_t MemoryRead8(uint64_t a) override { return rd<uint8_t>((uint32_t)a); }
    uint16_t MemoryRead16(uint64_t a) override { return rd<uint16_t>((uint32_t)a); }
    uint32_t MemoryRead32(uint64_t a) override { return rd<uint32_t>((uint32_t)a); }
    uint64_t MemoryRead64(uint64_t a) override { return rd<uint64_t>((uint32_t)a); }
    Dynarmic::A64::Vector MemoryRead128(uint64_t a) override { return {rd<uint64_t>((uint32_t)a), rd<uint64_t>((uint32_t)a + 8)}; }
    void MemoryWrite8(uint64_t a, uint8_t v) override { wr((uint32_t)a, v); }
    void MemoryWrite16(uint64_t a, uint16_t v) override { wr((uint32_t)a, v); }
    void MemoryWrite32(uint64_t a, uint32_t v) override { wr((uint32_t)a, v); }
    void MemoryWrite64(uint64_t a, uint64_t v) override { wr((uint32_t)a, v); }
    void MemoryWrite128(uint64_t a, Dynarmic::A64::Vector v) override { wr((uint32_t)a, v[0]); wr((uint32_t)a + 8, v[1]); }
    bool MemoryWriteExclusive8(uint64_t a, uint8_t v, uint8_t) override { wr((uint32_t)a, v); return true; }
    bool MemoryWriteExclusive16(uint64_t a, uint16_t v, uint16_t) override { wr((uint32_t)a, v); return true; }
    bool MemoryWriteExclusive32(uint64_t a, uint32_t v, uint32_t) override { wr((uint32_t)a, v); return true; }
    bool MemoryWriteExclusive64(uint64_t a, uint64_t v, uint64_t) override { wr((uint32_t)a, v); return true; }
    bool MemoryWriteExclusive128(uint64_t a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector) override { wr((uint32_t)a, v[0]); wr((uint32_t)a + 8, v[1]); return true; }
    // dynarmic's A64 front end lacks some ARMv8.1/8.3 instructions that newer compilers emit (LSE atomics, LDAPR). With one guest CPU running one
    // thread at a time they are plain loads and stores, so they are emulated here and execution continues at the next instruction.
    bool emulate_a64(uint64_t pc) {
        const uint32_t w = rd<uint32_t>((uint32_t)pc);
        auto reg = [&](int n) -> uint64_t { return n == 31 ? 0 : jit64->GetRegister(n); };
        auto setreg = [&](int n, uint64_t v) { if (n != 31) jit64->SetRegister(n, v); };
        const int size = (int)(w >> 30), bytes = 1 << size, rn = (w >> 5) & 31, rt = w & 31, rs = (w >> 16) & 31;
        const uint64_t mask = bytes == 8 ? ~0ull : (1ull << (bytes * 8)) - 1;
        const uint32_t addr = (uint32_t)(rn == 31 ? jit64->GetSP() : jit64->GetRegister(rn));
        auto load = [&]() { uint64_t v = 0; std::memcpy(&v, m + addr, bytes); return v; };
        auto store = [&](uint64_t v) { std::memcpy(m + addr, &v, bytes); };
        auto sext = [&](uint64_t v) { return bytes == 8 ? (int64_t)v : (int64_t)(v << (64 - bytes * 8)) >> (64 - bytes * 8); };
        if ((w & 0x3FFFFC00) == 0x38BFC000) { setreg(rt, load()); jit64->SetPC(pc + 4); return true; }                         // LDAPR{B,H,}: a load-acquire is stronger
        if ((w & 0x3F200C00) == 0x38200000) {                                                                                  // atomic memory operations
            const int o3 = (w >> 15) & 1, opc = (w >> 12) & 7; const uint64_t old = load(), v = reg(rs) & mask; uint64_t nv;
            if (o3 && opc == 0) nv = v;                                              // SWP
            else if (o3) return false;
            else switch (opc) {
                case 0: nv = old + v; break;                                         // LDADD
                case 1: nv = old & ~v; break;                                        // LDCLR
                case 2: nv = old ^ v; break;                                         // LDEOR
                case 3: nv = old | v; break;                                         // LDSET
                case 4: nv = sext(old) > sext(v) ? old : v; break;                   // LDSMAX
                case 5: nv = sext(old) < sext(v) ? old : v; break;                   // LDSMIN
                case 6: nv = old > v ? old : v; break;                               // LDUMAX
                default: nv = old < v ? old : v; break;                              // LDUMIN
            }
            store(nv & mask); setreg(rt, old); jit64->SetPC(pc + 4); return true;     // Rt = xzr is the ST* form: the old value is dropped
        }
        if ((w & 0x3FA07C00) == 0x08A07C00) {                                        // CAS{A,L}{B,H,}: compare Rs with memory, store Rt if equal, Rs = old
            const uint64_t old = load();
            if (old == (reg(rs) & mask)) store(reg(rt) & mask);
            setreg(rs, old); jit64->SetPC(pc + 4); return true;
        }
        return false;
    }
    void InterpreterFallback(uint64_t pc, size_t n) override {
        if (n == 1 && emulate_a64(pc)) return;
        char b[96]; std::snprintf(b, sizeof b, "interpreter fallback at %08llx (instruction %08x)", (unsigned long long)pc, rd<uint32_t>((uint32_t)pc)); fatal(b);
    }
    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception e) override {
        using E = Dynarmic::A64::Exception;
        if (e == E::Yield || e == E::SendEvent || e == E::SendEventLocal || e == E::WaitForEvent || e == E::WaitForInterrupt) return;   // hints
        char b[64]; std::snprintf(b, sizeof b, "exception %d at %08llx", (int)e, (unsigned long long)pc); fatal(b);
    }
    uint64_t GetCNTPCT() override { return 0; }
    // a guest that generates code (asmjit in the Osirus DSP emulator) flushes the instruction cache with `ic ivau` / `ic ialluis`: drop the translated blocks
    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, uint64_t value) override {
        if (op == Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU) jit64->InvalidateCacheRange(value & ~63ull, 64);
        else jit64->ClearCache();
    }
    void InterpreterFallback(uint32_t pc, size_t) override { char b[64]; std::snprintf(b, sizeof b, "interpreter fallback at %08x", pc); fatal(b); }
    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        if (e == E::PreloadData || e == E::PreloadDataWithIntentToWrite || e == E::PreloadInstruction || e == E::Yield ||
            e == E::SendEvent || e == E::SendEventLocal || e == E::WaitForEvent || e == E::WaitForInterrupt) return;   // hints
        char b[64]; std::snprintf(b, sizeof b, "exception %d at %08x", (int)e, pc); fatal(b);
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 40; }
    void CallSVC(uint32_t n) override {
        if (n == (a64 ? kRetSvc64 : kRetSvc)) { if (a64) jit64->HaltExecution(); else jit->HaltExecution(); return; }
        if (n >= thunks.size()) { fatal("bad svc " + std::to_string(n)); return; }
        static const bool trace = std::getenv("MPC_HOST_TRACE") != nullptr;   // every thunk call, with the guest thread
        if (trace) std::fprintf(stderr, "[t%u] %s(%x,%x,%x)\n", th.empty() ? 0 : th[cur]->id, thunk_names[n].c_str(), r(0), r(1), r(2));
        thunks[n](*this);
    }

    // --- cooperative guest threads ---
    // One JIT, one running thread at a time. A switch happens inside a thunk by swapping the register file, so the thread that
    // blocks resumes at the `bx lr` after its own svc. A new thread runs first, until it blocks or exits. Threads run only inside a
    // host call(); when the call returns, the runnable workers are drained once (drain()). Sleeping workers wake on the next call.
    enum St { Run, WaitMutex, WaitCond, WaitJoin, Sleep, Done };
    struct Ctx {   // a CPU register file: armhf r0-r15 + s0-s63 + CPSR/FPSCR, or aarch64 x0-x30, sp, pc, v0-v31, PSTATE/FPCR/FPSR
        std::array<uint32_t, 16> r{}; std::array<uint32_t, 64> ext{}; uint32_t cpsr = 0x10, fpscr = 0;
        std::array<uint64_t, 31> x{}; uint64_t sp = 0, pc = 0; std::array<Dynarmic::A64::Vector, 32> v{}; uint32_t pstate = 0, fpcr = 0, fpsr = 0;
    };
    void save_ctx(Ctx& c) {
        if (!a64) { c.r = jit->Regs(); c.ext = jit->ExtRegs(); c.cpsr = jit->Cpsr(); c.fpscr = jit->Fpscr(); return; }
        c.x = jit64->GetRegisters(); c.sp = jit64->GetSP(); c.pc = jit64->GetPC(); c.v = jit64->GetVectors();
        c.pstate = jit64->GetPstate(); c.fpcr = jit64->GetFpcr(); c.fpsr = jit64->GetFpsr();
    }
    void load_ctx(const Ctx& c) {
        if (!a64) { jit->Regs() = c.r; jit->ExtRegs() = c.ext; jit->SetCpsr(c.cpsr); jit->SetFpscr(c.fpscr); jit->ClearExclusiveState(); return; }
        jit64->SetRegisters(c.x); jit64->SetSP(c.sp); jit64->SetPC(c.pc); jit64->SetVectors(c.v);
        jit64->SetPstate(c.pstate); jit64->SetFpcr(c.fpcr); jit64->SetFpsr(c.fpsr); jit64->ClearExclusiveState();
    }
    struct GThread {
        uint32_t id = 0; St st = Run;
        Ctx ctx;
        uint32_t ret0 = 0; bool has_ret0 = false;   // the value the blocking call returns when the thread resumes
        gaddr obj = 0, mtx = 0, join_ret = 0;   // what it waits on, the mutex to take again after a cond wait, where join stores the result
        bool signaled = false, timed = false;
        uint32_t retval = 0, sleep_call = 0;
    };
    std::vector<std::unique_ptr<GThread>> th;
    size_t cur = 0;
    uint32_t call_no = 0, switches = 0;
    // MPC_HOST_STATS=1: host time spent running each guest thread (a slice starts when its context is loaded and ends at the next switch or halt)
    std::chrono::steady_clock::time_point slice_start; std::vector<double> busy_us; bool stats = std::getenv("MPC_HOST_STATS") != nullptr;
    void slice_begin() { if (stats) slice_start = std::chrono::steady_clock::now(); }
    void slice_end() {
        if (!stats) return;
        const size_t id = th.empty() ? 0 : th[cur]->id;
        if (busy_us.size() <= id) busy_us.resize(id + 1, 0.0);
        busy_us[id] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - slice_start).count();
        slice_start = std::chrono::steady_clock::now();
    }
    bool main_active = false;
    gaddr thread_exit_stub = 0;

    GThread& me() { return *th[cur]; }
    // mutex layout (glibc armhf): count at +4, owner at +8, kind at +12 (1 = recursive)
    uint32_t kind_off() const { return a64 ? 16 : 12; }   // glibc aarch64: lock +0, count +4, owner +8, nusers +12, kind +16
    bool mutex_free(gaddr mu) { return rd<uint32_t>(mu + 8) == 0; }
    bool try_lock(gaddr mu, uint32_t tid) {
        const uint32_t owner = rd<uint32_t>(mu + 8);
        if (owner == 0) { wr<uint32_t>(mu + 8, tid); wr<uint32_t>(mu + 4, 1); return true; }
        if (owner == tid && rd<uint32_t>(mu + kind_off()) == 1) { wr<uint32_t>(mu + 4, rd<uint32_t>(mu + 4) + 1); return true; }
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
        case WaitMutex: return mutex_free(t.obj) || (rd<uint32_t>(t.obj + kind_off()) == 1 && rd<uint32_t>(t.obj + 8) == t.id);
        case WaitCond: return t.signaled && mutex_free(t.mtx);
        case WaitJoin: return th[t.obj - 1]->st == Done;
        case Sleep: return call_no > t.sleep_call;
        case Done: return false;
        }
        return false;
    }
    void save(GThread& t) { slice_end(); save_ctx(t.ctx); }   // (cur is still the thread being switched out)
    void load(GThread& t) { load_ctx(t.ctx); slice_start = std::chrono::steady_clock::now(); }
    void apply_ret0(GThread& t) { if (t.has_ret0) { ret(t.ret0); t.has_ret0 = false; } }
    // The wait ended: take the mutex, store a join result, and make the blocking call return `r0`.
    void finish_wait(GThread& t, uint32_t r0) {
        switch (t.st) {
        case WaitMutex: try_lock(t.obj, t.id); break;
        case WaitCond: try_lock(t.mtx, t.id); break;
        case WaitJoin: if (t.join_ret) wrp(t.join_ret, th[t.obj - 1]->retval); break;
        default: break;
        }
        if (t.st != Run) { t.ret0 = r0; t.has_ret0 = true; }
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
                apply_ret0(t);
                return true;
            }
            bool sleepers = false;   // time passes only while the main thread waits inside a call; in drain() a sleeper waits for the next call
            for (auto& t : th) if (t->st == Sleep) sleepers = true;
            if (!sleepers || !main_active) break;
            call_no++;
        }
        if (main_active && th[0]->st != Done && th[0]->st != Run) {   // deadlock or a timed wait: the main thread gives up waiting
            GThread& t = *th[0];
            if (t.st == WaitMutex || (t.st == WaitCond && !mutex_free(t.mtx))) { fatal("deadlock (main thread waits on a held mutex)"); return false; }
            if (cur != 0) { save(me()); cur = 0; load(t); }
            finish_wait(t, t.timed ? 110 : 0);
            apply_ret0(t);
            return true;
        }
        return false;
    }
    // Blocks the current thread in state `st`; returns once something else ran, or immediately if it is the only thread that can.
    void block(St st, gaddr obj) {
        GThread& t = me();
        t.st = st; t.obj = obj; t.sleep_call = call_no;
        static const bool trace = std::getenv("MPC_HOST_TRACE") != nullptr;
        if (trace) std::fprintf(stderr, "[sched] t%u blocks: %s obj=%x%s\n", t.id, st == WaitMutex ? "mutex" : st == WaitCond ? "cond" : st == WaitJoin ? "join" : "sleep", obj,
                                st == WaitMutex ? (" owner=t" + std::to_string(rd<uint32_t>(obj + 8))).c_str() : "");
        if (!schedule()) { save(t); halt(); }   // a worker with nothing to do: back to the host
    }
    void yield() {
        if (th.size() < 2) return;
        GThread& t = me(); t.st = Run;
        const size_t before = cur;
        for (size_t k = 1; k < th.size(); k++) {
            const size_t j = (before + k) % th.size();
            if ((j == 0 && !main_active) || th[j]->st == Done || !ready(*th[j])) continue;
            save(t); cur = j; load(*th[j]); finish_wait(*th[j], 0); apply_ret0(*th[j]);
            return;
        }
    }
    uint32_t spawn(gaddr fn, uint32_t arg) {
        auto t = std::make_unique<GThread>();
        t->id = (uint32_t)th.size() + 1;
        const gaddr stack = malloc_(512 * 1024);
        if (!a64) {
            t->ctx.r[0] = arg; t->ctx.r[13] = (stack + 512 * 1024 - 16) & ~7u; t->ctx.r[14] = thread_exit_stub; t->ctx.r[15] = fn & ~1u;
            t->ctx.cpsr = 0x10 | ((fn & 1) ? 0x20 : 0);
        } else {
            t->ctx.x[0] = arg; t->ctx.sp = (stack + 512 * 1024 - 16) & ~15u; t->ctx.x[30] = thread_exit_stub; t->ctx.pc = fn;
        }
        const uint32_t id = t->id;
        th.push_back(std::move(t));
        // child first: the parent stays runnable and gets 0 back from pthread_create when the child blocks or exits
        ret(0);
        save(me()); cur = th.size() - 1; load(*th.back());
        return id;
    }
    void exit_thread(uint32_t retval) {
        GThread& t = me(); t.st = Done; t.retval = retval;
        if (!schedule()) { halt(); }
    }
    // After a host call returns: let workers that can run do so once. Registers the host call needs are put back.
    void drain() {
        if (th.size() < 2) return;
        Ctx host_ctx; save_ctx(host_ctx);
        switches = 0;
        for (int round = 0; round < 1000; round++) {
            size_t w = 0;
            for (size_t j = 1; j < th.size(); j++) if (th[j]->st != Done && ready(*th[j])) { w = j; break; }
            if (!w) break;
            cur = w; load(*th[w]); finish_wait(*th[w], 0); apply_ret0(*th[w]);
            slice_begin(); depth++; run(); depth--; slice_end();
            if (faulted) break;
        }
        cur = 0;
        load_ctx(host_ctx);
    }

    // --- argument access: armhf AAPCS (r0-r3, then the stack; floats in VFP registers) or aarch64 AAPCS64 (x0-x7; floats in v0-v7).
    // Thunks use the armhf names: r(0..3) are the first arguments, r(13) the stack pointer, r(14) the link register; on aarch64 they read x0-x7, sp, x30.
    // An aarch64 guest keeps all addresses below 2 GB, so a 32-bit value is a whole pointer, and a sign-extended int return is also a correct long/pointer.
    void halt() { if (a64) jit64->HaltExecution(); else jit->HaltExecution(); }
    bool faulted = false;
    std::string fault_msg;
    // The guest did something fatal: remember why, stop the CPU. Never abort the process (it may be a DAW); Guest::call throws GuestFault once the CPU is out of the JIT.
    void fatal(const std::string& msg) {
        std::fprintf(stderr, "guest: %s\n", msg.c_str());
        if (!faulted) { faulted = true; fault_msg = msg; }
        halt();
    }
    // Run until the guest asks to stop (a thunk halts, or the return stub). A cache invalidation (guest-generated code flushed with `ic ivau`) also
    // makes dynarmic's Run() return: that is not a stop, so go on from where the guest was.
    void run() {
        using HR = Dynarmic::HaltReason;
        if (faulted) return;
        for (;;) {
            const HR hr = a64 ? jit64->Run() : jit->Run();
            if ((static_cast<uint32_t>(hr) & static_cast<uint32_t>(HR::UserDefined1)) == 0 && (static_cast<uint32_t>(hr) & static_cast<uint32_t>(HR::CacheInvalidation)) != 0) continue;
            break;
        }
        if (a64) jit64->ClearHalt(HR::UserDefined1); else jit->ClearHalt(HR::UserDefined1);
    }
    uint32_t r(int i) {
        if (!a64) return jit->Regs()[i];
        return (uint32_t)(i == 13 ? jit64->GetSP() : jit64->GetRegister(i == 14 ? 30 : i));
    }
    uint64_t x(int i) { return jit64->GetRegister(i); }   // a full 64-bit argument (aarch64)
    uint32_t arg(int i) {
        if (a64) return i < 8 ? r(i) : rd<uint32_t>((uint32_t)jit64->GetSP() + 8 * (i - 8));
        return i < 4 ? r(i) : rd<uint32_t>(r(13) + 4 * (i - 4));
    }
    float s(int i) { float f; if (a64) { const uint64_t u = jit64->GetVector(i)[0]; std::memcpy(&f, &u, 4); } else std::memcpy(&f, &jit->ExtRegs()[i], 4); return f; }
    double d(int i) {
        uint64_t u = a64 ? jit64->GetVector(i)[0] : (jit->ExtRegs()[2 * i] | (uint64_t)jit->ExtRegs()[2 * i + 1] << 32);
        double v; std::memcpy(&v, &u, 8); return v;
    }
    void ret(uint32_t v) { if (a64) jit64->SetRegister(0, (uint64_t)(int64_t)(int32_t)v); else jit->Regs()[0] = v; }
    void ret64(uint64_t v) { if (a64) jit64->SetRegister(0, v); else { jit->Regs()[0] = (uint32_t)v; jit->Regs()[1] = (uint32_t)(v >> 32); } }
    void ret_f(float f) { uint32_t u; std::memcpy(&u, &f, 4); if (a64) jit64->SetVector(0, {u, 0}); else jit->ExtRegs()[0] = u; }
    void ret_d(double v) { uint64_t u; std::memcpy(&u, &v, 8); if (a64) jit64->SetVector(0, {u, 0}); else { jit->ExtRegs()[0] = (uint32_t)u; jit->ExtRegs()[1] = (uint32_t)(u >> 32); } }
    char* str(gaddr a) { return reinterpret_cast<char*>(m + a); }

    gaddr add_thunk(const std::string& name, Thunk t) {
        const uint32_t n = (uint32_t)thunks.size();
        const gaddr at = kThunks + 8 * n;
        if (at + 8 > kData) { std::fprintf(stderr, "guest: thunk page full\n"); std::abort(); }
        if (a64) { wr<uint32_t>(at, 0xD4000001u | (n << 5)); wr<uint32_t>(at + 4, 0xD65F03C0u); }   // svc #n; ret
        else { wr<uint32_t>(at, 0xEF000000u | n); wr<uint32_t>(at + 4, 0xE12FFF1Eu); }              // svc #n; bx lr
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
            if (heap_next + c > (a64 ? kHeapEnd64 : kHeapEnd)) return 0;
            p = heap_next; heap_next += c; commit(p, p + c);
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
        int fpreg = 0;      // aarch64: next v register for a double
        uint32_t va = 0;    // aarch64: a va_list structure {stack, gr_top, vr_top, gr_offs, vr_offs} instead of live registers
        // armhf: variadic arguments are all in core registers or on the stack, 8-byte values 8-aligned. aarch64: integers in x0-x7, doubles in v0-v7, then 8-byte stack slots.
        uint64_t next_int() {   // aarch64
            if (va) {
                const int32_t gr = i.rd<int32_t>(va + 24);
                if (gr < 0) { const uint64_t v = i.rd<uint64_t>(i.rdp(va + 8) + gr); i.wr<int32_t>(va + 24, gr + 8); return v; }
                const uint32_t st = i.rdp(va); const uint64_t v = i.rd<uint64_t>(st); i.wrp(va, st + 8); return v;
            }
            if (reg < 8) return i.x(reg++);
            const uint64_t v = i.rd<uint64_t>(sp); sp += 8; return v;
        }
        uint32_t next32() { if (i.a64) return (uint32_t)next_int(); if (reg < 4) return i.r(reg++); const uint32_t v = i.rd<uint32_t>(sp); sp += 4; return v; }
        uint64_t next64() {
            if (i.a64) return next_int();
            if (reg < 4) { if (reg & 1) reg++; if (reg < 4) { const uint64_t v = i.r(reg) | (uint64_t)i.r(reg + 1) << 32; reg += 2; return v; } }
            sp = (sp + 7) & ~7u; const uint64_t v = i.rd<uint64_t>(sp); sp += 8; return v;
        }
        double next_double() {
            uint64_t u;
            if (!i.a64) u = next64();
            else if (va) {
                const int32_t vr = i.rd<int32_t>(va + 28);
                if (vr < 0) { u = i.rd<uint64_t>(i.rdp(va + 16) + vr); i.wr<int32_t>(va + 28, vr + 16); }
                else { const uint32_t st = i.rdp(va); u = i.rd<uint64_t>(st); i.wrp(va, st + 8); }
            } else if (fpreg < 8) u = i.jit64->GetVector(fpreg++)[0];
            else { u = i.rd<uint64_t>(sp); sp += 8; }
            double v; std::memcpy(&v, &u, 8); return v;
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
            while (std::strchr("hlLzjt", *f)) { if (*f == 'l' && f[1] == 'l') { lng = 2; f += 2; } else { if (*f == 'L') lng = 3; if (a64 && std::strchr("lzjt", *f)) lng = 2; f++; } }   // long and size_t are 64-bit on aarch64
            const char c = *f++;
            spec += (lng == 2 && std::strchr("diouxX", c)) ? std::string("ll") + c : std::string(1, c);
            switch (c) {
            case 'd': case 'i': if (lng == 2) std::snprintf(tmp, sizeof tmp, spec.c_str(), (long long)va.next64()); else std::snprintf(tmp, sizeof tmp, spec.c_str(), (int32_t)va.next32()); break;
            case 'o': case 'u': case 'x': case 'X': if (lng == 2) std::snprintf(tmp, sizeof tmp, spec.c_str(), (unsigned long long)va.next64()); else std::snprintf(tmp, sizeof tmp, spec.c_str(), va.next32()); break;
            case 'c': std::snprintf(tmp, sizeof tmp, spec.c_str(), (int)va.next32()); break;
            case 's': { const uint32_t p = va.next32(); std::snprintf(tmp, sizeof tmp, spec.c_str(), p ? str(p) : "(null)"); break; }
            case 'p': std::snprintf(tmp, sizeof tmp, "0x%x", va.next32()); break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double v = va.next_double();
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
                if (!skip) { const gaddr p = va.next32(); if (a64 && lng) wr<uint64_t>(p, (uint64_t)v); else wr<uint32_t>(p, (uint32_t)v); }
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
    VarArgs va_list_args(uint32_t ap) { return a64 ? VarArgs{*this, 8, 0, 0, ap} : VarArgs{*this, 4, ap}; }   // a va_list is one pointer on armhf, a 32-byte structure passed by reference on aarch64
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
    if (lax && n.find("stack smashed") == std::string::npos && n != "abort") { if (!seen[n]++) { std::fprintf(stderr, "guest: stub %s, lr=%08x\n", n.c_str(), i.r(14)); i.stubs_called.push_back(n); } i.ret(0); return; }
    char b[160]; std::snprintf(b, sizeof b, "called %s (not supported), lr=%08x", n.c_str(), i.r(14));
    i.fatal(b);
    i.ret(0);
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
    auto strtol_ = [](I& i) { char* e; const long v = std::strtol(i.str(i.r(0)), &e, (int)i.r(2)); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); if (i.a64) i.ret64((uint64_t)v); else i.ret((uint32_t)(int32_t)v); };
    auto strtoll_ = [](I& i) { char* e; const long long v = std::strtoll(i.str(i.r(0)), &e, (int)i.r(2)); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret64((uint64_t)v); };
    auto strtoull_ = [](I& i) { char* e; const unsigned long long v = std::strtoull(i.str(i.r(0)), &e, (int)i.r(2)); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret64((uint64_t)v); };
    add("strtoll", strtoll_); add("__isoc23_strtoll", strtoll_); add("strtoull", strtoull_); add("__isoc23_strtoull", strtoull_);
    add("strncasecmp", [](I& i) { i.ret((uint32_t)::strncasecmp(i.str(i.r(0)), i.str(i.r(1)), i.r(2))); });
    add("strncat", [](I& i) { std::strncat(i.str(i.r(0)), i.str(i.r(1)), i.r(2)); i.ret(i.r(0)); });
    add("strpbrk", [](I& i) { const char* p = std::strpbrk(i.str(i.r(0)), i.str(i.r(1))); i.ret(p ? (uint32_t)(reinterpret_cast<const uint8_t*>(p) - i.m) : 0); });
    add("fsync", [](I& i) { i.ret(0); });
    add("localtime_r", [](I& i) {   // UTC (no time zone database): struct tm { nine ints, gmtoff, zone }
        const int64_t t = i.a64 ? (int64_t)i.rd<int64_t>(i.r(0)) : (int64_t)i.rd<int32_t>(i.r(0));
        const std::time_t tt = (std::time_t)t; std::tm out{};
#ifdef _WIN32
        gmtime_s(&out, &tt);
#else
        gmtime_r(&tt, &out);
#endif
        const int v[9] = {out.tm_sec, out.tm_min, out.tm_hour, out.tm_mday, out.tm_mon, out.tm_year, out.tm_wday, out.tm_yday, 0};
        std::memset(i.m + i.r(1), 0, i.a64 ? 56 : 44);
        std::memcpy(i.m + i.r(1), v, sizeof v);
        i.ret(i.r(1)); });
    for (const char* n : {"accept4", "accept", "bind", "listen", "setsockopt", "getifaddrs"}) add(n, [](I& i) { i.ret(i.fail(97)); });   // EAFNOSUPPORT: no sockets for guests (the manager's local web page just stays off)
    add("freeifaddrs", [](I& i) { i.ret(0); });
    add("inet_ntop", [](I& i) { i.ret(0); });
    add("strtol", strtol_);
    add("__isoc23_strtol", strtol_);
    add("strtoul", [](I& i) { char* e; const unsigned long v = std::strtoul(i.str(i.r(0)), &e, (int)i.r(2)); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); if (i.a64) i.ret64((uint64_t)v); else i.ret((uint32_t)v); });
    add("__isoc23_strtoul", t.back().fn);
    add("strtod", [](I& i) { char* e; const double v = std::strtod(i.str(i.r(0)), &e); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_d(v); });
    add("strtof", [](I& i) { char* e; const float v = std::strtof(i.str(i.r(0)), &e); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_f(v); });
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
    add("__clock_gettime64", [](I& i) { int64_t sec, nsec; if (i.r(0) == 0) hostfs::realtime(sec, nsec); else hostfs::monotonic(sec, nsec); const gaddr p = i.r(1); i.wr<int64_t>(p, sec); i.wr<int32_t>(p + 8, (int32_t)nsec); i.wr<int32_t>(p + 12, 0); i.ret(0); });
    add("clock_gettime", [](I& i) { int64_t sec, nsec; if (i.r(0) == 0) hostfs::realtime(sec, nsec); else hostfs::monotonic(sec, nsec); const gaddr p = i.r(1); if (i.a64) { i.wr<int64_t>(p, sec); i.wr<int64_t>(p + 8, nsec); } else { i.wr<int32_t>(p, (int32_t)sec); i.wr<int32_t>(p + 4, (int32_t)nsec); } i.ret(0); });
    // threads: one guest thread, so locks are no-ops
    add("pthread_mutex_init", [](I& i) { std::memset(i.m + i.r(0), 0, i.a64 ? 48 : 24); if (i.r(1)) i.wr<uint32_t>(i.r(0) + i.kind_off(), i.rd<uint32_t>(i.r(1))); i.ret(0); });
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
    add("pthread_create", [](I& i) { const gaddr thr = i.r(0), fn = i.r(2), arg = i.r(3); const uint32_t id = i.th.size() + 1; i.wrp(thr, id); i.spawn(fn, arg); });
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
    add("raise", [](I& i) { i.fatal("raise(" + std::to_string(i.r(0)) + ")"); i.ret(0); });
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
    add("__strtod_l", [](I& i) { char* e; const double v = std::strtod(i.str(i.r(0)), &e); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_d(v); });
    add("strtold_l", [](I& i) { char* e; const double v = std::strtod(i.str(i.r(0)), &e); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_d(v); });
    add("__strtof_l", [](I& i) { char* e; const float v = std::strtof(i.str(i.r(0)), &e); if (i.r(1)) i.wrp(i.r(1), (uint32_t)(reinterpret_cast<uint8_t*>(e) - i.m)); i.ret_f(v); });
    add("__strcoll_l", [](I& i) { i.ret((uint32_t)std::strcmp(i.str(i.r(0)), i.str(i.r(1)))); });
    add("__strxfrm_l", [](I& i) { const size_t n = std::strlen(i.str(i.r(1))); if (i.r(2) > n) std::strcpy(i.str(i.r(0)), i.str(i.r(1))); i.ret((uint32_t)n); });
    add("isspace", [](I& i) { i.ret(std::isspace((int)i.r(0)) ? 1 : 0); });
    add("__errno_location", [](I& i) { if (!i.errno_p) i.errno_p = i.data(4, 4); i.ret(i.errno_p); });
    add("strerror", [](I& i) { i.ret(i.cstr("error")); });
    add("__assert_fail", [](I& i) { char b[256]; std::snprintf(b, sizeof b, "assert %s at %s:%u", i.str(i.r(0)), i.str(i.r(1)), i.r(2)); i.fatal(b); i.ret(0); });
    add("syscall", [](I& i) { i.ret(i.fail(38)); });   // ENOSYS: no raw system calls (a plugin using them falls back or fails)
    for (const char* n : {"pthread_cond_destroy", "pthread_cond_init", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
                          "pthread_rwlock_unlock", "pthread_detach", "__cxa_thread_atexit_impl"})
        add(n, [](I& i) { i.ret(0); });   // one thread: nothing to wait for or wake
    add("memchr", [](I& i) { const void* p = std::memchr(i.m + i.r(0), (int)i.r(1), i.r(2)); i.ret(p ? (uint32_t)(static_cast<const uint8_t*>(p) - i.m) : 0); });
    add("strdup", [](I& i) { i.ret(i.g.strdup(i.str(i.r(0)))); });
    add("posix_memalign", [](I& i) { const gaddr p = i.malloc_(i.r(2) + i.r(1)); i.wrp(i.r(0), (p + i.r(1) - 1) & ~(i.r(1) - 1)); i.ret(0); });
    add("__memcpy_chk", [](I& i) { std::memcpy(i.m + i.r(0), i.m + i.r(1), i.r(2)); i.ret(i.r(0)); });
    add("__memmove_chk", [](I& i) { std::memmove(i.m + i.r(0), i.m + i.r(1), i.r(2)); i.ret(i.r(0)); });
    add("__strcpy_chk", [](I& i) { std::strcpy(i.str(i.r(0)), i.str(i.r(1))); i.ret(i.r(0)); });
    add("wcslen", [](I& i) { uint32_t n = 0; while (i.rd<uint32_t>(i.r(0) + 4 * n)) n++; i.ret(n); });
    add("frexp", [](I& i) { int e; const double v = std::frexp(i.d(0), &e); i.wr<int32_t>(i.r(0), e); i.ret_d(v); });
    add("ldexp", [](I& i) { i.ret_d(std::ldexp(i.d(0), (int)i.r(0))); });
    add("ldexpf", [](I& i) { i.ret_f(std::ldexp(i.s(0), (int)i.r(0))); });
    add("modf", [](I& i) { double ip; const double v = std::modf(i.d(0), &ip); std::memcpy(i.m + i.r(0), &ip, 8); i.ret_d(v); });
    add("vsnprintf", [](I& i) { const gaddr b = i.r(0); const uint32_t n = i.r(1); i.put(b, n, i.format(i.str(i.r(2)), i.va_list_args(i.r(3)))); });
    add("vsprintf", [](I& i) { const std::string s = i.format(i.str(i.r(1)), i.va_list_args(i.r(2))); i.put(i.r(0), 0x7fffffff, s); });
    add("__vsprintf_chk", [](I& i) { const std::string s = i.format(i.str(i.arg(3)), i.va_list_args(i.arg(4))); i.put(i.r(0), 0x7fffffff, s); });
    add("__vsnprintf_chk", [](I& i) { const std::string s = i.format(i.str(i.arg(4)), i.va_list_args(i.arg(5))); i.put(i.r(0), i.r(1), s); });
    auto vfprintf_out = [](I& i, gaddr file, const std::string& s) { if (FILE* f = i.file_of(file)) std::fputs(s.c_str(), f); else std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); };
    add("vfprintf", [vfprintf_out](I& i) { vfprintf_out(i, i.r(0), i.format(i.str(i.r(1)), i.va_list_args(i.r(2)))); });
    add("__vfprintf_chk", [vfprintf_out](I& i) { vfprintf_out(i, i.r(0), i.format(i.str(i.r(2)), i.va_list_args(i.r(3)))); });
    add("vprintf", [](I& i) { const std::string s = i.format(i.str(i.r(0)), i.va_list_args(i.r(1))); std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    add("__vprintf_chk", [](I& i) { const std::string s = i.format(i.str(i.r(1)), i.va_list_args(i.r(2))); std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    add("signal", [](I& i) { i.ret(0); });   // SIG_DFL back: no signals are delivered to guests
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
        const gaddr dst = i.r(0), src = i.r(1), nms = i.r(2), len = i.r(3); const gaddr s0 = i.rdp(src);
        uint32_t n = 0;
        for (; n < nms && (!dst || n < len); n++) {
            const uint8_t c = i.m[s0 + n];
            if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
            if (dst) i.wr<uint32_t>(dst + 4 * n, c);
            if (!c) { if (dst) i.wrp(src, 0); i.ret(n); return; }
        }
        if (dst) i.wrp(src, s0 + n);
        i.ret(n); });
    add("__mbsrtowcs_chk", [](I& i) {
        const gaddr dst = i.r(0), src = i.r(1), len = i.r(2); const gaddr s0 = i.rdp(src);
        uint32_t n = 0;
        for (;; n++) {
            if (dst && n >= len) break;
            const uint8_t c = i.m[s0 + n];
            if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
            if (dst) i.wr<uint32_t>(dst + 4 * n, c);
            if (!c) { if (dst) i.wrp(src, 0); i.ret(n); return; }
        }
        i.wrp(src, s0 + n); i.ret(n); });
    add("wcsnrtombs", [](I& i) {
        const gaddr dst = i.r(0), src = i.r(1), nwc = i.r(2), len = i.r(3); const gaddr s0 = i.rdp(src);
        uint32_t n = 0;
        for (; n < nwc && (!dst || n < len); n++) {
            const uint32_t c = i.rd<uint32_t>(s0 + 4 * n);
            if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
            if (dst) i.m[dst + n] = (uint8_t)c;
            if (!c) { if (dst) i.wrp(src, 0); i.ret(n); return; }
        }
        if (dst) i.wrp(src, s0 + 4 * n);
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
    add("time", [](I& i) { const uint32_t t = (uint32_t)std::time(nullptr); if (i.r(0)) { if (i.a64) i.wr<uint64_t>(i.r(0), t); else i.wr<uint32_t>(i.r(0), t); } i.ret(t); });
    // single-import stoppers found by the survey: calls that can fail or do nothing for a sandboxed guest
    add("frexpf", [](I& i) { int e; const float v = std::frexp(i.s(0), &e); i.wr<int32_t>(i.r(0), e); i.ret_f(v); });
    add("fma", [](I& i) { i.ret_d(std::fma(i.d(0), i.d(1), i.d(2))); });
    add("fmaf", [](I& i) { i.ret_f(std::fma(i.s(0), i.s(1), i.s(2))); });
    add("localtime", [](I& i) { i.ret(0); });   // callers log timestamps: a NULL result is allowed by the C standard
    // dlopen: only a libcurl lookalike, and only when the embedding host lends HTTP (the shim is guest code in guest_helpers.c; see curl_easy_* there)
    add("dlopen", [](I& i) { const std::string n = i.r(0) ? i.str(i.r(0)) : ""; i.ret(n.find("libcurl") != std::string::npos && i.g.hooks.http_get ? 1 : 0); });
    add("dlsym", [](I& i) { i.ret(i.r(0) == 1 && i.r(1) ? i.g.symbol(i.str(i.r(1))) : 0); });
    add("dlerror", [](I& i) { i.ret(0); });
    add("__mpc_http_open", [](I& i) {   // (url, from): the whole body is fetched now; a handle, or -1
        std::vector<uint8_t> body; std::string err;
        if (!i.g.hooks.http_get || !i.g.hooks.http_get(i.str(i.r(0)), body, err)) { i.ret(0xFFFFFFFFu); return; }
        const int h = i.next_http++;
        size_t from = i.r(1);
        i.http[h] = {std::move(body), from};
        i.ret((uint32_t)h); });
    add("__mpc_http_total", [](I& i) { auto it = i.http.find((int)i.r(0)); i.ret(it == i.http.end() ? 0 : (uint32_t)it->second.first.size()); });
    add("__mpc_http_read", [](I& i) {   // (handle, buf, n) -> bytes
        auto it = i.http.find((int)i.r(0));
        if (it == i.http.end()) { i.ret(0); return; }
        auto& b = it->second;
        const size_t n = b.second >= b.first.size() ? 0 : std::min<size_t>(i.r(2), b.first.size() - b.second);
        if (n) std::memcpy(i.m + i.r(1), b.first.data() + b.second, n);
        b.second += n;
        i.ret((uint32_t)n); });
    add("__mpc_http_close", [](I& i) { i.http.erase((int)i.r(0)); i.ret(0); });
    // posix_spawn(p): the argument vector goes to the embedding host's spawn hook, which runs it now; waitpid hands back the status
    for (const char* n : {"posix_spawn", "posix_spawnp"}) add(n, [](I& i) {
        if (!i.g.hooks.spawn) { i.ret(38); return; }   // ENOSYS (posix_spawn returns the error number)
        std::vector<std::string> argv;
        const uint32_t ps = i.a64 ? 8 : 4;
        for (uint32_t a = i.r(4); a && argv.size() < 64; a += ps) { const uint32_t sp = i.rdp(a); if (!sp) break; argv.push_back(i.str(sp)); }
        if (argv.empty()) argv.push_back(i.r(1) ? i.str(i.r(1)) : "");
        const int status = i.g.hooks.spawn(i.g, argv);
        const int pid = i.next_pid++;
        i.exits[pid] = status;
        if (i.r(0)) i.wr<int32_t>(i.r(0), pid);
        i.ret(status == -1000 ? 2u : 0u); });   // -1000: no such program (ENOENT)
    add("waitpid", [](I& i) {
        auto it = i.exits.find((int)i.r(0));
        if (it == i.exits.end()) { i.ret(i.fail(10)); return; }   // ECHILD
        if (i.r(1)) i.wr<int32_t>(i.r(1), (it->second & 0xff) << 8);
        const int pid = it->first; i.exits.erase(it); i.ret((uint32_t)pid); });
    // statvfs: plenty of free space on every path that exists (the guest's "disk" is the host's folder)
    for (const char* n : {"statvfs", "statvfs64"}) add(n, [](I& i) {
        std::string hp;
        if (!i.hostpath(i.str(i.r(0)), hp) || !std::filesystem::exists(hp)) { i.ret(i.fail(2)); return; }
        if (i.a64) {
            std::memset(i.m + i.r(1), 0, 112);
            const uint64_t v[11] = {4096, 4096, 1ull << 24, 1ull << 23, 1ull << 23, 1ull << 20, 1ull << 19, 1ull << 19, 0, 0, 255};
            std::memcpy(i.m + i.r(1), v, sizeof v);
        } else {   // armhf statvfs: eleven 32-bit fields
            std::memset(i.m + i.r(1), 0, 44);
            const uint32_t v[11] = {4096, 4096, 1u << 22, 1u << 21, 1u << 21, 1u << 20, 1u << 19, 1u << 19, 0, 0, 255};
            std::memcpy(i.m + i.r(1), v, sizeof v);
        }
        i.ret(0); });
    for (const char* n : {"sigaction", "pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setschedpolicy", "pthread_attr_setschedparam",
                          "pthread_attr_setinheritsched", "pthread_setname_np", "pthread_setaffinity_np", "sched_setaffinity", "pthread_setschedparam"})
        add(n, [](I& i) { i.ret(0); });
    for (const char* n : {"getrlimit", "snd_seq_open"}) add(n, [](I& i) { i.ret(0xFFFFFFFFu); });
    add("strtok_r", [](I& i) {
        gaddr p = i.r(0) ? i.r(0) : i.rdp(i.r(2));
        const char* delim = i.str(i.r(1));
        if (!p) { i.ret(0); return; }
        while (i.m[p] && std::strchr(delim, i.m[p])) p++;
        if (!i.m[p]) { i.wrp(i.r(2), p); i.ret(0); return; }
        const gaddr tok = p;
        while (i.m[p] && !std::strchr(delim, i.m[p])) p++;
        if (i.m[p]) { i.m[p] = 0; p++; }
        i.wrp(i.r(2), p);
        i.ret(tok); });
    add("strftime", [](I& i) { if (i.r(1)) i.m[i.r(0)] = 0; i.ret(0); });   // no clock formatting: an empty string
    for (const char* n : {"setrlimit", "pthread_attr_setstacksize", "pthread_attr_setdetachstate"}) add(n, [](I& i) { i.ret(0); });
    add("aligned_alloc", [](I& i) { const gaddr p = i.malloc_(i.r(1) + i.r(0)); i.ret((p + i.r(0) - 1) & ~(i.r(0) - 1)); });
    add("setenv", [](I& i) { i.ret(0); });
    add("bcmp", [](I& i) { i.ret((uint32_t)std::memcmp(i.m + i.r(0), i.m + i.r(1), i.r(2))); });
    for (const char* n : {"pthread_getschedparam", "pthread_getattr_np", "pthread_attr_getstack", "pthread_attr_getguardsize", "sched_getaffinity"}) add(n, [](I& i) { i.ret(0xFFFFFFFFu); });
    for (const char* n : {"socketpair", "socket", "pipe"}) add(n, [](I& i) { i.ret(0xFFFFFFFFu); });
    add("gettimeofday", [](I& i) { int64_t sec, nsec; hostfs::realtime(sec, nsec); const int64_t usec = nsec / 1000; if (i.r(0)) { if (i.a64) { i.wr<int64_t>(i.r(0), sec); i.wr<int64_t>(i.r(0) + 8, usec); } else { i.wr<int32_t>(i.r(0), (int32_t)sec); i.wr<int32_t>(i.r(0) + 4, (int32_t)usec); } } i.ret(0); });
    // ARM EHABI: the unwinder asks which exception index table covers a pc (glibc provides this, built on dl_iterate_phdr)
    add("__gnu_Unwind_Find_exidx", [](I& i) {
        for (auto& md : i.mods) if (i.r(0) >= md.base && i.r(0) < md.end && md.exidx) { if (i.r(1)) i.wr<uint32_t>(i.r(1), md.exidx_size / 8); i.ret(md.exidx); return; }
        if (i.r(1)) i.wr<uint32_t>(i.r(1), 0);
        i.ret(0); });
    // --- files (see Impl::hostpath): stdio on host FILE*s, fds are host fds, directories, stat, glob ---
    auto out_file = [](I& i, gaddr f) -> FILE* { FILE* h = i.file_of(f); return h ? h : stderr; };
    auto fopen_ = [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.set_errno(2); i.ret(0); return; }
        std::string mode = i.str(i.r(1)); if (mode.find('b') == std::string::npos) mode += 'b';   // the guest expects Linux semantics: no newline translation
        FILE* f = std::fopen(hp.c_str(), mode.c_str());
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
    add("fseek", [](I& i) { FILE* f = i.file_of(i.r(0)); const long off = i.a64 ? (long)i.x(1) : (long)(int32_t)i.r(1); i.ret(f ? (uint32_t)std::fseek(f, off, (int)i.r(2)) : 0xFFFFFFFFu); });
    add("ftell", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret64(f ? (uint64_t)(int64_t)std::ftell(f) : ~0ull); });
    add("fseeko64", [](I& i) { FILE* f = i.file_of(i.r(0)); const int64_t off = (int64_t)((uint64_t)i.r(2) | (uint64_t)i.r(3) << 32); i.ret(f ? (uint32_t)hostfs::fseek64(f, off, (int)i.arg(4)) : 0xFFFFFFFFu); });
    add("ftello64", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret64(f ? (uint64_t)hostfs::ftell64(f) : ~0ull); });
    add("fprintf", [](I& i) { const std::string s = i.format(i.str(i.r(1)), {i, 2, i.r(13)}); if (FILE* f = i.file_of(i.r(0))) std::fputs(s.c_str(), f); else std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    add("__fprintf_chk", [](I& i) { const std::string s = i.format(i.str(i.r(2)), {i, 3, i.r(13)}); if (FILE* f = i.file_of(i.r(0))) std::fputs(s.c_str(), f); else std::fprintf(stderr, "[guest] %s", s.c_str()); i.ret((uint32_t)s.size()); });
    auto open_ = [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(2)); return; }
        const int fd = hostfs::open(hp, i.r(1), i.r(2));   // the guest's Linux O_* flags are translated there
        i.ret(fd < 0 ? i.fail(errno) : (uint32_t)fd); };
    add("open", open_); add("open64", open_);
    add("__open64_2", open_); add("__openat64_2", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(1)), hp)) { i.ret(i.fail(2)); return; } const int fd = hostfs::open(hp, i.r(2) & 3, 0); i.ret(fd < 0 ? i.fail(errno) : (uint32_t)fd); });
    add("close", [](I& i) { i.ret(i.r(0) > 2 ? (uint32_t)hostfs::close((int)i.r(0)) : 0); });
    add("read", [](I& i) { const long long n = hostfs::read((int)i.r(0), i.m + i.r(1), i.r(2)); i.ret(n < 0 ? i.fail(errno) : (uint32_t)n); });
    add("__read_chk", [](I& i) { const long long n = hostfs::read((int)i.r(0), i.m + i.r(1), i.r(2)); i.ret(n < 0 ? i.fail(errno) : (uint32_t)n); });
    add("write", [](I& i) { const int fd = (int)i.r(0) == 1 ? 2 : (int)i.r(0); const long long n = hostfs::write(fd, i.m + i.r(1), i.r(2)); i.ret(n < 0 ? i.fail(errno) : (uint32_t)n); });
    add("lseek64", [](I& i) { const int64_t off = (int64_t)((uint64_t)i.r(2) | (uint64_t)i.r(3) << 32); i.ret64((uint64_t)hostfs::lseek((int)i.r(0), off, (int)i.arg(4))); });
    add("access", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(2)); return; } i.ret(hostfs::access(hp, (int)i.r(1)) ? i.fail(errno) : 0); });
    add("mkdir", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(13)); return; } const int e = hostfs::mkdir(hp); i.ret(e ? i.fail(e) : 0); });
    add("remove", [](I& i) { std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(i.fail(13)); return; } i.ret(std::remove(hp.c_str()) ? i.fail(errno) : 0); });
    add("rename", [](I& i) { std::string a, b; if (!i.hostpath(i.str(i.r(0)), a) || !i.hostpath(i.str(i.r(1)), b)) { i.ret(i.fail(13)); return; } i.ret(std::rename(a.c_str(), b.c_str()) ? i.fail(errno) : 0); });
    add("getcwd", [](I& i) { if (i.r(1) < 2) { i.ret(0); return; } std::memcpy(i.m + i.r(0), "/", 2); i.ret(i.r(0)); });
    // armhf `struct stat` (88 bytes, 32-bit st_size at 44) for __xstat/__lxstat/__fxstat, and `struct stat64` (st_size 64-bit at 48) for the *64 forms
    auto put_stat = [](I& i, gaddr a, const hostfs::Stat& st, int mode) {   // mode 0: stat, 1: stat64, 2: stat with 64-bit times (glibc 2.34+, 112 bytes), 3: aarch64
        std::memset(i.m + a, 0, mode == 0 ? 88 : mode == 1 ? 104 : mode == 2 ? 112 : 128);   // exactly the guest struct: it sits on the guest stack
        i.wr<uint32_t>(a + 16, st.mode); i.wr<uint32_t>(a + 20, st.nlink); i.wr<uint32_t>(a + 24, st.uid); i.wr<uint32_t>(a + 28, st.gid);
        i.wr<uint64_t>(a, st.dev); i.wr<uint64_t>(a + 32, st.rdev);
        if (mode == 3) {   // aarch64 struct stat: ino +8, size +48, blksize +56, blocks +64, three {sec, nsec} pairs of 64-bit words from +72
            i.wr<uint64_t>(a + 8, st.ino); i.wr<uint64_t>(a + 48, st.size); i.wr<uint32_t>(a + 56, st.blksize); i.wr<uint64_t>(a + 64, st.blocks);
            i.wr<int64_t>(a + 72, st.atime); i.wr<int64_t>(a + 88, st.mtime); i.wr<int64_t>(a + 104, st.ctime);
        } else if (mode == 2) {
            i.wr<uint64_t>(a + 8, st.ino); i.wr<uint64_t>(a + 40, st.size); i.wr<uint32_t>(a + 48, st.blksize); i.wr<uint64_t>(a + 56, st.blocks);
            i.wr<int64_t>(a + 64, st.atime); i.wr<int64_t>(a + 80, st.mtime); i.wr<int64_t>(a + 96, st.ctime);
        } else if (mode == 1) {
            i.wr<uint32_t>(a + 12, (uint32_t)st.ino); i.wr<uint64_t>(a + 48, st.size); i.wr<uint32_t>(a + 56, st.blksize); i.wr<uint64_t>(a + 64, st.blocks);
            i.wr<uint32_t>(a + 72, (uint32_t)st.atime); i.wr<uint32_t>(a + 80, (uint32_t)st.mtime); i.wr<uint32_t>(a + 88, (uint32_t)st.ctime); i.wr<uint64_t>(a + 96, st.ino);
        } else {
            i.wr<uint32_t>(a + 12, (uint32_t)st.ino); i.wr<uint32_t>(a + 44, (uint32_t)st.size); i.wr<uint32_t>(a + 48, st.blksize); i.wr<uint32_t>(a + 52, (uint32_t)st.blocks);
            i.wr<uint32_t>(a + 56, (uint32_t)st.atime); i.wr<uint32_t>(a + 64, (uint32_t)st.mtime); i.wr<uint32_t>(a + 72, (uint32_t)st.ctime); i.wr<uint64_t>(a + 80, st.ino);
        }
    };
    // stat by path (path argument at index `pa`, buffer at pa + 1) and by descriptor
    auto stat_path = [put_stat](I& i, int pa, bool follow, int mode) {
        std::string hp; if (!i.hostpath(i.str(i.r(pa)), hp)) { i.ret(i.fail(2)); return; }
        hostfs::Stat st; if (!hostfs::stat(hp, st, follow)) { i.ret(i.fail(errno ? errno : 2)); return; }
        put_stat(i, i.r(pa + 1), st, mode); i.ret(0); };
    auto stat_fd = [put_stat](I& i, int fa, int mode) {
        hostfs::Stat st; if (!hostfs::fstat((int)i.r(fa), st)) { i.ret(i.fail(errno)); return; }
        put_stat(i, i.r(fa + 1), st, mode); i.ret(0); };
    for (const char* n : {"__xstat", "__xstat64", "__lxstat", "__lxstat64"}) {   // (ver, path, buf)
        const int mode = std::strstr(n, "64") != nullptr ? 1 : 0; const bool lst = std::strstr(n, "lx") != nullptr;
        add(n, [mode, lst, stat_path](I& i) { stat_path(i, 1, !lst, i.a64 ? 3 : mode); });
    }
    for (const char* n : {"__stat64_time64", "__lstat64_time64"}) {   // (path, buf)
        const bool lst = std::strstr(n, "lstat") != nullptr;
        add(n, [lst, stat_path](I& i) { stat_path(i, 0, !lst, 2); });
    }
    for (const char* n : {"__fxstat", "__fxstat64"}) {
        const int mode = std::strstr(n, "64") != nullptr ? 1 : 0;
        add(n, [mode, stat_fd](I& i) { stat_fd(i, 1, i.a64 ? 3 : mode); });
    }
    for (const char* n : {"stat", "stat64", "lstat", "lstat64"}) {   // aarch64 glibc exports these directly (older glibc via __xstat)
        const bool lst = n[0] == 'l';
        add(n, [lst, stat_path](I& i) { stat_path(i, 0, !lst, 3); });
    }
    for (const char* n : {"fstat", "fstat64"}) add(n, [stat_fd](I& i) { stat_fd(i, 0, 3); });
    // long and off_t are 64-bit on aarch64: offsets come in whole registers
    add("fseeko", [](I& i) { FILE* f = i.file_of(i.r(0)); const int64_t off = i.a64 ? (int64_t)i.x(1) : (int32_t)i.r(1); i.ret(f ? (uint32_t)hostfs::fseek64(f, off, (int)i.r(2)) : 0xFFFFFFFFu); });
    add("ftello", [](I& i) { FILE* f = i.file_of(i.r(0)); i.ret64(f ? (uint64_t)hostfs::ftell64(f) : ~0ull); });
    add("lseek", [](I& i) { const int64_t off = i.a64 ? (int64_t)i.x(1) : (int32_t)i.r(1); i.ret64((uint64_t)hostfs::lseek((int)i.r(0), off, (int)i.r(2))); });
    add("__fstat64_time64", [stat_fd](I& i) { stat_fd(i, 0, 2); });
    // fscanf: read ahead into a buffer, scan it, and seek back over what was not consumed (regular files)
    auto fscanf_ = [](I& i) {
        FILE* f = i.file_of(i.r(0)); if (!f) { i.ret(0xFFFFFFFFu); return; }
        const long long pos = hostfs::ftell64(f); char buf[4096]; const size_t got = std::fread(buf, 1, sizeof buf - 1, f); buf[got] = 0;
        const int n = i.scan(buf, i.str(i.r(1)), {i, 2, i.r(13)});
        if (pos >= 0) hostfs::fseek64(f, pos + (long long)i.scan_used, SEEK_SET);
        i.ret((uint32_t)n); };
    for (const char* n : {"fscanf", "__isoc99_fscanf", "__isoc23_fscanf"}) add(n, fscanf_);
    // directories: DIR* is a guest handle; the dirent lives in a 288-byte guest buffer (armhf dirent: d_ino +0, d_type +10, d_name +11; dirent64: d_ino +0, d_type +18, d_name +19)
    add("opendir", [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.set_errno(2); i.ret(0); return; }
        hostfs::Dir d; if (!hostfs::listdir(hp, d)) { i.set_errno(2); i.ret(0); return; }
        const gaddr h = i.malloc_(288); i.dirs[h] = std::move(d); i.ret(h); });
    auto readdir_ = [](I& i, bool is64) {
        auto it = i.dirs.find(i.r(0)); if (it == i.dirs.end() || it->second.pos >= it->second.ents.size()) { i.ret(0); return; }
        const auto& e = it->second.ents[it->second.pos]; const uint64_t ino = ++it->second.pos;
        const gaddr b = i.r(0); std::memset(i.m + b, 0, 288);
        if (is64) { i.wr<uint64_t>(b, ino); i.wr<uint16_t>(b + 16, 280); i.m[b + 18] = e.second; std::strncpy((char*)i.m + b + 19, e.first.c_str(), 255); }
        else { i.wr<uint32_t>(b, (uint32_t)ino); i.wr<uint16_t>(b + 8, 280); i.m[b + 10] = e.second; std::strncpy((char*)i.m + b + 11, e.first.c_str(), 255); }
        i.ret(b); };
    add("readdir", [readdir_](I& i) { readdir_(i, i.a64); });   // aarch64 has only the 64-bit dirent
    add("readdir64", [readdir_](I& i) { readdir_(i, true); });
    add("closedir", [](I& i) { auto it = i.dirs.find(i.r(0)); if (it != i.dirs.end()) { i.dirs.erase(it); i.free_(i.r(0)); } i.ret(0); });
    // glob_t (armhf): gl_pathc +0, gl_pathv +4, gl_offs +8
    auto glob_ = [](I& i) {
        std::string hp; if (!i.hostpath(i.str(i.r(0)), hp)) { i.ret(3); return; }
        const std::vector<std::string> found = hostfs::glob(hp); if (found.empty()) { i.ret(3); return; }   // GLOB_NOMATCH
        const uint32_t ps = i.a64 ? 8 : 4;   // glob_t: gl_pathc, gl_pathv, gl_offs, each pointer-sized
        const gaddr vec = i.malloc_(ps * ((uint32_t)found.size() + 1));
        for (size_t k = 0; k < found.size(); k++) i.wrp(vec + ps * (uint32_t)k, i.g.strdup(i.guestpath(found[k]).c_str()));
        i.wrp(vec + ps * (uint32_t)found.size(), 0);
        i.wrp(i.r(3), (uint32_t)found.size()); i.wrp(i.r(3) + ps, vec); i.wrp(i.r(3) + 2 * ps, 0);
        i.ret(0); };
    for (const char* n : {"glob", "glob64", "__glob64_time64"}) add(n, glob_);
    for (const char* n : {"globfree", "globfree64", "__globfree64_time64"}) add(n, [](I& i) { i.ret(0); });   // the vectors are left in the guest heap
    // dladdr: Dl_info { fname, fbase, sname, saddr }; /plugin/<name> for the loaded plugin, /lib/<name> for libraries
    add("dladdr", [](I& i) {
        for (size_t k = 0; k < i.mods.size(); k++) {
            if (i.r(0) < i.mods[k].base || i.r(0) >= i.mods[k].end) continue;
            const std::string nm = std::filesystem::path(i.mod_paths[k]).filename().string();
            const bool is_lib = k + 1 < i.mods.size() && std::getenv("MPC_HOST_LIBS");   // libraries load before the plugin
            const uint32_t ps = i.a64 ? 8 : 4; i.wrp(i.r(1), i.g.strdup(((is_lib ? "/lib/" : "/plugin/") + nm).c_str())); i.wrp(i.r(1) + ps, i.mods[k].base); i.wrp(i.r(1) + 2 * ps, 0); i.wrp(i.r(1) + 3 * ps, 0);
            i.ret(1); return;
        }
        i.ret(0); });
    for (const char* n : {"getpwnam", "getpwuid", "getenv_s"}) add(n, [](I& i) { i.ret(0); });   // no user database
    // --- process and system queries, memory mapping, text and spawn stubs (both architectures) ---
    for (const char* n : {"getauxval", "__getauxval"}) add(n, [](I& i) { i.ret(0); });
    add("getpid", [](I& i) { i.ret(1000); });
    add("getpagesize", [](I& i) { i.ret(4096); });
    add("sysconf", [](I& i) { const uint32_t n = i.r(0); i.ret(n == 30 ? 4096 : n == 84 || n == 83 ? 1 : n == 2 ? 100 : 0xFFFFFFFFu); });   // _SC_PAGESIZE, _SC_NPROCESSORS_*, _SC_CLK_TCK
    add("get_nprocs", [](I& i) { i.ret(1); });
    add("mmap", [](I& i) {   // anonymous mappings only, from the guest heap, page aligned
        const uint32_t len = i.r(1), flags = i.r(3);
        if (!(flags & 0x20) || len == 0) { i.ret(i.fail(19)); return; }   // MAP_ANONYMOUS (0x20 on armhf and aarch64)
        const gaddr p = i.malloc_(len + 4096);
        if (!p) { i.ret(i.fail(12)); return; }
        const gaddr a = (p + 4095) & ~4095u; std::memset(i.m + a, 0, len); i.ret(a); });
    for (const char* n : {"munmap", "mprotect", "mlock", "munlock", "madvise", "msync"}) add(n, [](I& i) { i.ret(0); });
    add("toupper", [](I& i) { i.ret((uint32_t)std::toupper((int)i.r(0))); });
    add("tolower", [](I& i) { i.ret((uint32_t)std::tolower((int)i.r(0))); });
    add("strcspn", [](I& i) { i.ret((uint32_t)std::strcspn(i.str(i.r(0)), i.str(i.r(1)))); });
    add("strspn", [](I& i) { i.ret((uint32_t)std::strspn(i.str(i.r(0)), i.str(i.r(1)))); });
    add("gettext", [](I& i) { i.ret(i.r(0)); });
    add("dgettext", [](I& i) { i.ret(i.r(1)); });
    for (const char* n : {"bindtextdomain", "bind_textdomain_codeset", "textdomain"}) add(n, [](I& i) { i.ret(0); });
    add("arc4random", [](I& i) { i.ret((uint32_t)std::rand()); });
    add("getentropy", [](I& i) { for (uint32_t k = 0; k < i.r(1); k++) i.m[i.r(0) + k] = (uint8_t)std::rand(); i.ret(0); });
    for (const char* n : {"ftruncate", "ftruncate64", "writev", "ioctl", "iconv_open", "shm_open", "shm_unlink", "unlink", "fdopen", "fileno"})
        add(n, [](I& i) { i.ret(i.fail(38)); });   // ENOSYS
    for (const char* n : {"posix_spawn_file_actions_init", "posix_spawn_file_actions_destroy", "posix_spawn_file_actions_addopen", "posix_spawn_file_actions_adddup2", "iconv_close"}) add(n, [](I& i) { i.ret(0); });
    // glibc 2.35+: struct dl_find_object { u64 flags; void *map_start, *map_end, *link_map, *eh_frame; ... } (aarch64 layout); libgcc's unwinder uses it
    add("_dl_find_object", [](I& i) {
        static const bool off = std::getenv("MPC_HOST_NO_DLFO") != nullptr;   // test switch: make the unwinder fall back to dl_iterate_phdr
        if (off) { i.ret(0xFFFFFFFFu); return; }
        for (auto& md : i.mods) if (i.r(0) >= md.base && i.r(0) < md.end && md.eh) {
            const gaddr r = i.r(1); std::memset(i.m + r, 0, 64);
            i.wr<uint64_t>(r + 8, md.base); i.wr<uint64_t>(r + 16, md.end); i.wr<uint64_t>(r + 32, md.eh);
            i.ret(0); return;
        }
        i.ret(0xFFFFFFFFu); });
    add("mbsrtowcs", [](I& i) {
        const gaddr dst = i.r(0), src = i.r(1), len = i.r(2); const gaddr s0 = i.rdp(src);
        uint32_t n = 0;
        for (;; n++) {
            if (dst && n >= len) break;
            const uint8_t c = i.m[s0 + n];
            if (c >= 128) { i.ret(0xFFFFFFFFu); return; }
            if (dst) i.wr<uint32_t>(dst + 4 * n, c);
            if (!c) { if (dst) i.wrp(src, 0); i.ret(n); return; }
        }
        i.wrp(src, s0 + n); i.ret(n); });
    for (const char* n : {"setpriority", "getpriority", "nice", "setpgid", "sigaltstack", "sigemptyset", "sigaddset", "sigprocmask", "pthread_sigmask", "prctl"}) add(n, [](I& i) { i.ret(0); });
    for (const char* n : {"getuid", "geteuid", "getgid", "getegid"}) add(n, [](I& i) { i.ret(1000); });
    add("uname", [](I& i) {   // struct utsname: six 65-byte fields
        std::memset(i.m + i.r(0), 0, 6 * 65);
        const char* v[5] = {"Linux", "mpc", "5.15.0", "#1", i.a64 ? "aarch64" : "armv7l"};
        for (int k = 0; k < 5; k++) std::strcpy((char*)i.m + i.r(0) + 65 * k, v[k]);
        i.ret(0); });
    for (const char* n : {"readlink", "statfs", "statfs64", "fstatvfs", "realpath"}) add(n, [](I& i) { i.ret(i.fail(2)); });   // no symlinks, no filesystem statistics
#define ISW(n) add(#n, [](I& i) { i.ret(i.r(0) < 128 && std::n((wint_t)i.r(0)) ? 1 : 0); });   // wide-character classes: ASCII only (the "C" locale)
    ISW(iswspace) ISW(iswalpha) ISW(iswdigit) ISW(iswalnum) ISW(iswupper) ISW(iswlower) ISW(iswpunct) ISW(iswprint) ISW(iswcntrl) ISW(iswxdigit)
#undef ISW
    add("towlower", [](I& i) { i.ret(i.r(0) < 128 ? (uint32_t)std::tolower((int)i.r(0)) : i.r(0)); });
    add("towupper", [](I& i) { i.ret(i.r(0) < 128 ? (uint32_t)std::toupper((int)i.r(0)) : i.r(0)); });
    add("gnu_get_libc_version", [](I& i) { i.ret(i.cstr("2.39")); });
    add("__aeabi_read_tp", [](I& i) { i.ret(kTp); });
    add("__tls_get_addr", [](I& i) { const gaddr ti = i.r(0); const uint32_t off = i.rd<uint32_t>(ti + (i.a64 ? 8 : 4)); i.ret(i.tls.at(i.rd<uint32_t>(ti) - 1).block + off); });   // tls_index {module, offset}
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
bool is_data_import(const std::string& n) { return n == "__libc_single_threaded" || n == "__stack_chk_guard" || n == "_ZSt7nothrow" || n == "stderr" || n == "stdout"; }
}  // namespace

namespace {
// One import of a module being loaded: a symbol of a module loaded earlier, a data object, a table thunk, or an abort stub (recorded in `missing`).
gaddr resolve_import(I& i, std::vector<std::string>& missing, std::unordered_map<std::string, Thunk>& table, const std::string& name, bool weak) {
    if (auto d = i.dynsyms.find(name); d != i.dynsyms.end()) return d->second;   // a library loaded before this one
    if (is_data_import(name)) {
        auto& da = i.data_by_name[name];
        if (da) return da;
        const gaddr a = da = i.data(64);
        if (name == "__stack_chk_guard") { i.wr<uint32_t>(a, 0xC0FFEE00u); if (i.a64) i.wr<uint32_t>(a + 4, 0xC0FFEE00u); }
        if (name == "__libc_single_threaded") i.m[a] = 0;   // threads exist (cooperative), so report "not single threaded"
        return a;
    }
    if ((name == "__ctype_b_loc" || name == "__ctype_tolower_loc" || name == "__ctype_toupper_loc") && !i.thunk_by_name.count(name)) {
        // glibc's tables are indexed from -128; armhf, aarch64 and x86-64 glibc are little-endian with the same contents
        const bool b = name == "__ctype_b_loc"; const uint32_t esz = b ? 2 : 4;
        const gaddr tbl = i.data(384 * esz, 8), pp = i.data(8, 8);
        if (b) hostfs::ctype_b(reinterpret_cast<uint16_t*>(i.m + tbl));
        else hostfs::ctype_case(reinterpret_cast<int32_t*>(i.m + tbl), name == "__ctype_toupper_loc");
        i.wrp(pp, tbl + 128 * esz);
        return i.thunk_for(name, [pp](I& ii) { ii.ret(pp); });
    }
    if (name == "dl_iterate_phdr") return i.dl_iterate_phdr_stub();
    if (name == "pthread_once") {   // guest code, so the init routine runs without a nested call: if (!*once) { *once = 1; tail-call init }
        if (auto t = i.thunk_by_name.find(name); t != i.thunk_by_name.end()) return t->second;
        return i.thunk_by_name[name] = i.a64
            ? i.stub({0xB9400002, 0x35000082, 0x52800022, 0xB9000002, 0xD61F0020, 0xD65F03C0})   // ldr w2,[x0]; cbnz w2,ret; mov w2,#1; str w2,[x0]; br x1; ret
            : i.stub({0xE5902000, 0xE3520000, 0x112FFF1E, 0xE3A02001, 0xE5802000, 0xE12FFF11});
    }
    auto it = table.find(name);
    if (it != table.end()) return i.thunk_for(name, it->second);
    if (weak) return 0;
    missing.push_back(name);
    return i.thunk_for(name, [name](I& ii) { abort_named(ii, name); });
}
}  // namespace

Guest::Guest(bool a64) {
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, kSpace, MEM_RESERVE, PAGE_READWRITE);
    if (!p) { std::fprintf(stderr, "cannot reserve 4 GB of address space (error %lu)\n", GetLastError()); std::abort(); }
#else
    void* p = mmap(nullptr, kSpace, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap 4 GB"); std::abort(); }
#endif
    mem_ = static_cast<uint8_t*>(p);
    impl_ = new Impl(*this, a64);
    impl_->commit(0, kLoad);                                                                  // thunks, data, TLS and the first load address
    impl_->commit((a64 ? kStackTop64 : kStackTop) - 8u * 1024 * 1024, a64 ? kStackTop64 : kStackTop);   // the main stack
    // index 0: a trap at guest address 0 (a call through a null pointer, e.g. an unresolved weak import)
    impl_->add_thunk("<null>", [](I& i) { char b[64]; std::snprintf(b, sizeof b, "call to address 0 from lr=%08x", i.r(14)); i.fatal(b); i.ret(0); });
    impl_->wr<uint32_t>(0, a64 ? 0xD4000001u : 0xEF000000u);   // svc #0 at address 0
    impl_->th.push_back(std::make_unique<Guest::Impl::GThread>());
    impl_->th[0]->id = 1;
    impl_->thread_exit_stub = impl_->add_thunk("<thread exit>", [](I& i) { i.exit_thread(i.r(0)); });
    const gaddr ret = kData - 8;
    impl_->wr<uint32_t>(ret, a64 ? (0xD4000001u | (kRetSvc64 << 5)) : (0xEF000000u | kRetSvc));

    if (a64) {
        Dynarmic::A64::UserConfig cfg;
        cfg.callbacks = impl_;
        cfg.global_monitor = &impl_->monitor;
        cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(mem_);
        cfg.fastmem_address_space_bits = 32;        // the guest uses the low 2 GB of its 4 GB arena
        cfg.silently_mirror_fastmem = true;
        cfg.tpidr_el0 = &impl_->tpidr_el0;
        cfg.enable_cycle_counting = false;
        if (std::getenv("MPC_HOST_UNSAFE")) {
            cfg.unsafe_optimizations = true;
            cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                                 Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN | Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
        }
        impl_->tpidr_el0 = kTp;
        impl_->jit64 = std::make_unique<Dynarmic::A64::Jit>(cfg);
        return;
    }
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
    if (impl_->stats) {   // MPC_HOST_STATS=1
        double total = 0; for (double us : impl_->busy_us) total += us;
        for (size_t id = 0; id < impl_->busy_us.size(); id++) if (impl_->busy_us[id] > 0) std::fprintf(stderr, "stats: guest thread t%zu ran %.0f ms (%.0f%%)\n", id, impl_->busy_us[id] / 1000, 100 * impl_->busy_us[id] / total);
    }
    delete impl_;
#ifdef _WIN32
    VirtualFree(mem_, 0, MEM_RELEASE);
#else
    munmap(mem_, kSpace);
#endif
}

bool Guest::is64() const { return impl_->a64; }
bool Guest::faulted() const { return impl_->faulted; }
bool Guest::host_path(const std::string& gp, std::string& host) { return impl_->hostpath(gp.c_str(), host); }
const std::vector<std::string>& Guest::stubs_called() const { return impl_->stubs_called; }
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
        const float opt = i.s(0);
        i.ret(fn(i.g, i.r(0), (int32_t)i.r(1), (int32_t)i.r(2), (int32_t)i.r(3), i.arg(4), opt));
    });
}

uint32_t Guest::call(gaddr fn, const std::vector<uint32_t>& ints, const std::vector<float>& floats) {
    I& i = *impl_;
    if (i.faulted) throw GuestFault(i.fault_msg);
    if (i.depth) { i.fault_msg = "nested call from a thunk (not supported)"; i.faulted = true; std::fprintf(stderr, "guest: %s\n", i.fault_msg.c_str()); throw GuestFault(i.fault_msg); }
    if (i.a64) {   // AAPCS64: integer arguments in x0-x7 (then 8-byte stack slots), floats in v0-v7
        auto& J = *i.jit64;
        Guest::Impl::Ctx saved; i.save_ctx(saved);
        uint64_t sp = saved.sp ? saved.sp : kStackTop64;
        const size_t nstack = ints.size() > 8 ? ints.size() - 8 : 0;
        sp -= 8 * nstack; sp &= ~15ull;
        for (size_t k = 0; k < nstack; k++) i.wr<uint64_t>((uint32_t)sp + 8 * (uint32_t)k, ints[8 + k]);
        for (size_t k = 0; k < 8 && k < ints.size(); k++) J.SetRegister(k, ints[k]);
        for (size_t k = 0; k < floats.size() && k < 8; k++) { uint32_t u; std::memcpy(&u, &floats[k], 4); J.SetVector(k, {u, 0}); }
        J.SetSP(sp); J.SetRegister(30, kData - 8); J.SetPC(fn);
        i.call_no++; i.cur = 0; i.main_active = true;
        i.slice_begin();
        i.depth++; i.run(); i.depth--;
        i.slice_end();
        i.main_active = false;
        if (i.faulted) throw GuestFault(i.fault_msg);
        const uint64_t x0 = J.GetRegister(0); const auto v0 = J.GetVector(0);   // the result: x0, or v0 for a float/double
        i.drain();
        if (i.faulted) throw GuestFault(i.fault_msg);
        i.load_ctx(saved);
        J.SetRegister(0, x0); J.SetVector(0, v0);
        return (uint32_t)x0;
    }
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
    i.slice_begin();
    i.depth++;
    i.jit->Run();
    i.jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
    i.slice_end();
    i.depth--;
    i.main_active = false;
    if (i.faulted) throw GuestFault(i.fault_msg);
    const uint32_t r0 = R[0];
    const uint32_t s0 = i.jit->ExtRegs()[0], s1 = i.jit->ExtRegs()[1];   // a double result lives in s0/s1
    i.drain();
    if (i.faulted) throw GuestFault(i.fault_msg);
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
    if (impl_->a64) return load64(path);
    I& i = *impl_;
    const gaddr bias = i.load_next;
    std::ifstream f(path, std::ios::binary);
    if (!f) { err_ = "cannot open " + path; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    if (file.size() < sizeof(Elf32_Ehdr)) { err_ = "not an ELF file"; return false; }
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(file.data());   // (an aarch64 file is refused below: use Guest(true))
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM || eh->e_type != ET_DYN) {
        err_ = "not a 32-bit ARM shared object (an aarch64 build needs Guest(true))"; return false;
    }
    const auto* ph = reinterpret_cast<const Elf32_Phdr*>(file.data() + eh->e_phoff);
    const Elf32_Dyn* dyn = nullptr;
    uint32_t end = 0, tls_id = 0;
    std::pair<gaddr, uint32_t> exidx{0, 0};
    for (int k = 0; k < eh->e_phnum; k++) {
        if (ph[k].p_type == PT_LOAD) end = std::max(end, ph[k].p_vaddr + ph[k].p_memsz);
        if (ph[k].p_type == PT_ARM_EXIDX) exidx = {bias + ph[k].p_vaddr, ph[k].p_memsz};
        if (ph[k].p_type == PT_LOAD) {
            i.commit(bias + ph[k].p_vaddr, bias + ph[k].p_vaddr + ph[k].p_memsz);
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
    i.register_phdr(bias, path, file.data() + eh->e_phoff, eh->e_phnum, eh->e_phentsize);
    if (std::getenv("MPC_HOST_TRACE")) std::fprintf(stderr, "module %s at %08x-%08x\n", path.c_str(), bias, bias + end);
    i.plugin_dir = Guest::Impl::slashes(std::filesystem::path(path).parent_path().generic_string());
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
        symaddr[k] = resolve_import(i, missing_, table, name, ELF32_ST_BIND(syms[k].st_info) == STB_WEAK);
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

// The aarch64 loader: ELF64 shared objects with RELA relocations; TLS variant 1 with a 16-byte TCB (TPIDR_EL0 = kTp).
bool Guest::load64(const std::string& path) {
    I& i = *impl_;
    const gaddr bias = i.load_next;
    std::ifstream f(path, std::ios::binary);
    if (!f) { err_ = "cannot open " + path; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    if (file.size() < sizeof(Elf64_Ehdr)) { err_ = "not an ELF file"; return false; }
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(file.data());
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_AARCH64 || eh->e_type != ET_DYN) {
        err_ = "not a 64-bit aarch64 shared object (an armhf build needs Guest(false))"; return false;
    }
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(file.data() + eh->e_phoff);
    const Elf64_Dyn* dyn = nullptr;
    uint64_t end = 0, eh_frame = 0; uint32_t tls_id = 0;
    for (int k = 0; k < eh->e_phnum; k++) {
        if (ph[k].p_type == PT_GNU_EH_FRAME) eh_frame = ph[k].p_vaddr;
        if (ph[k].p_type == PT_LOAD) {
            end = std::max<uint64_t>(end, ph[k].p_vaddr + ph[k].p_memsz);
            if (bias + ph[k].p_vaddr + ph[k].p_memsz >= 0x70000000u) { err_ = "module does not fit below 1.75 GB"; return false; }
            i.commit(bias + (uint32_t)ph[k].p_vaddr, bias + (uint32_t)(ph[k].p_vaddr + ph[k].p_memsz));
            std::memcpy(mem_ + bias + ph[k].p_vaddr, file.data() + ph[k].p_offset, ph[k].p_filesz);
            std::memset(mem_ + bias + ph[k].p_vaddr + ph[k].p_filesz, 0, ph[k].p_memsz - ph[k].p_filesz);
        } else if (ph[k].p_type == PT_TLS) {
            const uint32_t al = (uint32_t)std::max<uint64_t>(ph[k].p_align, 1);
            i.tls_used = (i.tls_used + al - 1) / al * al;
            const gaddr blk = kTp + 16 + i.tls_used;
            if (blk + ph[k].p_memsz > kLoad) { err_ = "TLS area full"; return false; }
            std::memset(mem_ + blk, 0, ph[k].p_memsz);
            std::memcpy(mem_ + blk, file.data() + ph[k].p_offset, ph[k].p_filesz);
            i.tls.push_back({blk, i.tls_used});
            i.tls_used += (uint32_t)ph[k].p_memsz;
            tls_id = (uint32_t)i.tls.size();
        } else if (ph[k].p_type == PT_DYNAMIC) dyn = reinterpret_cast<const Elf64_Dyn*>(mem_ + bias + ph[k].p_vaddr);
    }
    if (!dyn) { err_ = "no PT_DYNAMIC"; return false; }
    i.load_next = (bias + (uint32_t)end + 0xFFFFu) & ~0xFFFFu;
    i.mods.push_back({bias, bias + (uint32_t)end, 0, 0, eh_frame ? bias + (uint32_t)eh_frame : 0});
    i.mod_paths.push_back(path);
    i.register_phdr(bias, path, file.data() + eh->e_phoff, eh->e_phnum, eh->e_phentsize);
    if (std::getenv("MPC_HOST_TRACE")) std::fprintf(stderr, "module %s at %08x-%08x\n", path.c_str(), bias, bias + (uint32_t)end);
    i.plugin_dir = Guest::Impl::slashes(std::filesystem::path(path).parent_path().generic_string());
    uint64_t symtab = 0, strtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, init = 0, init_array = 0, init_arraysz = 0;
    for (const Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
        case DT_STRTAB: strtab = d->d_un.d_ptr; break;
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_INIT: init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_un.d_val; break;
        case DT_REL: err_ = "REL relocations (not aarch64)"; return false;
        case 36 /* DT_RELR */: err_ = "DT_RELR relative relocations are not supported yet"; return false;
        }
    }
    uint32_t nsyms = 0;
    const auto* sh = reinterpret_cast<const Elf64_Shdr*>(file.data() + eh->e_shoff);
    for (int k = 0; k < eh->e_shnum; k++) if (sh[k].sh_type == SHT_DYNSYM) nsyms = (uint32_t)(sh[k].sh_size / sizeof(Elf64_Sym));
    const auto* syms = reinterpret_cast<const Elf64_Sym*>(mem_ + bias + symtab);
    const char* strs = reinterpret_cast<const char*>(mem_ + bias + strtab);

    std::unordered_map<std::string, Thunk> table;
    for (auto& e : libc_table()) table.emplace(e.name, std::move(e.fn));
    std::vector<gaddr> symaddr(nsyms, 0);
    const size_t missing_before = missing_.size();
    for (uint32_t k = 1; k < nsyms; k++) {
        const std::string name = strs + syms[k].st_name;
        if (syms[k].st_shndx != SHN_UNDEF) {
            symaddr[k] = bias + (uint32_t)syms[k].st_value;
            if (ELF64_ST_TYPE(syms[k].st_info) == STT_TLS) symaddr[k] = (uint32_t)syms[k].st_value;   // an offset in this module's TLS block
            if (ELF64_ST_BIND(syms[k].st_info) != STB_LOCAL && ELF64_ST_TYPE(syms[k].st_info) != STT_TLS) i.dynsyms.emplace(name, symaddr[k]);
            continue;
        }
        if (name.empty()) continue;
        symaddr[k] = resolve_import(i, missing_, table, name, ELF64_ST_BIND(syms[k].st_info) == STB_WEAK);
    }
    for (size_t k = missing_before; k < missing_.size(); k++) std::fprintf(stderr, "  no thunk for import: %s\n", missing_[k].c_str());

    // a TLS descriptor's resolver is guest code that returns the offset stored next to it: ldr x0,[x0,#8]; ret
    const gaddr tlsdesc_stub = i.thunk_by_name.count("<tlsdesc>") ? i.thunk_by_name["<tlsdesc>"] : (i.thunk_by_name["<tlsdesc>"] = i.stub({0xF9400400, 0xD65F03C0}));
    auto apply = [&](uint64_t at, uint64_t size) -> bool {
        for (uint64_t off = 0; off < size; off += sizeof(Elf64_Rela)) {
            const auto* rr = reinterpret_cast<const Elf64_Rela*>(mem_ + bias + at + off);
            const uint32_t type = ELF64_R_TYPE(rr->r_info), sym = ELF64_R_SYM(rr->r_info);
            const gaddr P = bias + (uint32_t)rr->r_offset;
            const uint64_t S = sym < nsyms ? symaddr[sym] : 0, A = (uint64_t)rr->r_addend;
            const uint64_t tpoff = 16 + (tls_id ? i.tls[tls_id - 1].off : 0);
            switch (type) {
            case R_AARCH64_RELATIVE: i.wr<uint64_t>(P, bias + A); break;
            case R_AARCH64_GLOB_DAT: case R_AARCH64_JUMP_SLOT: i.wr<uint64_t>(P, S + A); break;
            case R_AARCH64_ABS64: i.wr<uint64_t>(P, S + A); break;
            case R_AARCH64_NONE: break;
            case R_AARCH64_TLS_DTPMOD: i.wr<uint64_t>(P, tls_id); break;
            case R_AARCH64_TLS_DTPREL: i.wr<uint64_t>(P, S + A); break;
            case R_AARCH64_TLS_TPREL: i.wr<uint64_t>(P, S + A + tpoff); break;
            case R_AARCH64_TLSDESC: i.wr<uint64_t>(P, tlsdesc_stub); i.wr<uint64_t>(P + 8, S + A + tpoff); break;
            default: err_ = "unsupported relocation type " + std::to_string(type); return false;
            }
        }
        return true;
    };
    if (!apply(rela, relasz) || !apply(jmprel, pltrelsz)) return false;

    if (init) call(bias + (uint32_t)init, {});
    for (uint64_t k = 0; k < init_arraysz / 8; k++) {
        const uint64_t fn = i.rd<uint64_t>(bias + (uint32_t)init_array + 8 * (uint32_t)k);
        if (fn && fn != ~0ull) call((gaddr)fn, {});
    }
    return true;
}

}  // namespace mpchost

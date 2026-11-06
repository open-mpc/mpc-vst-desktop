# MPC plugin host for desktop DAWs (idea and import survey)

> Moved from mpc-vst-plugins on 2026-11-06. Paths to `docs/NOTES.md`, `docs/CATALOG.md`, `docs/EMULATOR_PORTS.md`, `tools/build_port.sh`, `tools/gen_vst.py`, `tools/studio.py`, `wrapper/engine.h`, `wrapper/vst2_wrap.c`, `adapters/` and `poc/<probe>/` are in that repo: the `mpc-vst-plugins/` submodule here.

**Status (2026-11-06): a Linux x86-64 prototype plays Acid-303's released-style armv7 `.so` under dynarmic (offline; see "Prototype"). No VST3 shell yet.**

The goal: one VST3 for desktop DAWs on Windows and macOS (Linux too) that loads MPC plugin `.so` files *as released*
(armv7 Gen1/Force, aarch64 Gen2) and browses them like MPC's plugin list. No second build of each plugin. This is a
different thing from `docs/DESKTOP_VST3.md`, which rebuilds a port from source as a native VST3. The host matters most
for plugins that only exist as MPC binaries.

## Approach

A plugin is an ARM Linux ELF shared object. It cannot be `dlopen`ed into an x86 or macOS DAW, and running Linux in a VM
(WSL2, Hyper-V, Virtualization.framework) is a heavy install with poor real-time behaviour. The plan is user-mode emulation
inside our own process, as console and Android emulators do:

1. **CPU:** dynarmic (AArch32 and AArch64 guest; x86-64 and arm64 host JIT; 0BSD). Unicorn as a slower fallback.
2. **Loader:** our own ELF loader maps the `.so` into a reserved 4 GB guest region and applies its relocations. armv7
   code needs 32-bit pointers.
3. **Imports go to native code.** Calls into libc, libm and libpthread are thunked to host functions: plain calls map 1:1,
   and `pthread_create` starts a host thread with its own guest CPU context. C++ ports can run Debian's armhf/arm64
   `libstdc++` as guest code, so only libc is thunked. Akai's files are never shipped.
4. **VST2 ABI:** `VSTPluginMain` runs as guest code. The host callback and the AEffect calls are thunks both ways (the
   ABI is already hand-written in `wrapper/vst2_wrap.c`).
5. **UI and library:** native. The skin renderer in `wrapper/vst3_editor.c` draws the plugin's `TUI.json` skin; it needs
   HWND and NSView window layers (see DESKTOP_VST3.md "No editor"). The browser reads the installed plugin folders and
   `mpc-plugin.json` (docs/CATALOG.md).

**Process layout.** On macOS a JIT needs `MAP_JIT` and the `com.apple.security.cs.allow-jit` entitlement. A hardened DAW
does not give that to a plugin loaded into it. So the emulator runs in our own signed helper process, and the VST3 is a
thin shell that talks to it over shared memory, with a ring one block ahead of the DAW (as in docs/EMULATOR_PORTS.md).
Use the same layout on Windows for crash isolation.

## Import survey (2026-11-05, offline)

Other people's plugin repos (and their release zips) were not reachable from the session that wrote this, so only this
repo's probes were measured. `poc/sampleprobe`, `poc/songpos` and `poc/inputprobe` were built for armv7 with Ubuntu's
`arm-linux-gnueabihf-gcc` (glibc 2.39 headers, not the bullseye image `build_port.sh` uses), using the same flags and
`wrapper/vst2_wrap.c`. Their undefined dynamic symbols:

- **NEEDED:** `libc.so.6`, `ld-linux-armhf.so.3`, plus `libm.so.6` for inputprobe.
- **Every probe** (the wrapper's floor, about 25 symbols): `calloc free memcpy memset strcmp strlen strncpy strtod strtol
  snprintf raise`, `pthread_mutex_{init,destroy,lock,unlock}`, `pthread_mutexattr_{init,destroy,settype,setprotocol}`,
  fortify and stack-protector helpers (`__snprintf_chk __stack_chk_fail __stack_chk_guard`), and the CRT weak symbols
  (`__cxa_finalize __gmon_start__ _ITM_*`).
- **inputprobe also:** `getenv fopen64 fflush __fprintf_chk __vfprintf_chk strcasecmp __ctype_b_loc clock_gettime`, and
  from libm `ceilf floorf fmaxf lroundf roundf`.
- A bullseye build gives the older names (`strtol`, not `__isoc23_strtol`; `clock_gettime`, not `__clock_gettime64`).
  The thunk table should cover both.

The wrapper is small and plain. Real engines will add more than the probes, and these are the known extras that need
special handling, not a 1:1 thunk:

| Use | Where | What the host does |
|---|---|---|
| `fopen("/proc/self/maps")` to find its own folder | `wrapper/plugin_dir.h` | serve a fake maps file naming the plugin's path |
| `/sys/devices/system/cpu/*/cpufreq` and `sched_setaffinity` | `wrapper/fast_cores.h` | fake one fast cluster; make affinity a no-op |
| `dlopen("libcurl.so.4")` for HTTPS | NOTES.md "HTTPS" | a native HTTP implementation behind libcurl's API, or refuse |
| ALSA sequencer MIDI out (`snd_seq_*`) | NOTES.md | route to the host's MIDI out, or drop |
| `/sdcard/...` and other absolute paths | ports | map to a library folder |

## Three real ports (2026-11-06, offline, from source)

Release zips were not downloadable from the session (HTTP 403), so the repos were cloned and read. Acid-303 was also built
for armv7 with Ubuntu's cross g++ (same cflags as its `vst.json`, `-mcpu=cortex-a17 -mfpu=neon-vfpv4`). They show three
tiers of difficulty:

- **Acid-303 (`sd88me/mpc-vst-acid303`): easy.** Its NEEDED libraries are libstdc++, libm, libgcc_s and libc. About 65
  imports: the probe set plus libm (`exp exp2 log log1p pow sin tan lrintf lround`), `memmove`, `strchr`, and the C++
  runtime (`operator new`/`delete`, `__cxa_*` exception and guard helpers, `__gxx_personality_v0`,
  `__aeabi_unwind_cpp_pr0/1`). The C++ runtime runs as guest code. One thing to check: the engine uses NEON intrinsics
  (`voice.cpp`, `__ARM_NEON`), so dynarmic's AArch32 ASIMD coverage has to include them.
- **Machinemodule (`sd88me/mpc-vst-machinedrum`): hard.**
  - Two plugins share state. The tap effects find the Module with `dlopen("machinedrum_one.so", RTLD_NOLOAD)`, or
    through `/proc/self/maps`, and use its statics. So every plugin instance must live in one guest address space, and
    the loader must answer `RTLD_NOLOAD` lookups.
  - The voice engine runs on parallel threads pinned with `sched_setaffinity`.
  - Once per flash image it runs a helper program, `posix_spawn("/bin/sh", "ulimit -s 8192; exec helper ...")`. The host
    would have to run a second guest executable, or ship the files that helper makes.
  - The Gen1 build is made by the user from their own firmware; no binary is released. The Gen2 aarch64 build runs the
    dsp56300 JIT, which writes arm64 code at run time. Under dynarmic that is a JIT inside a JIT: it needs `mmap`/`mprotect`
    thunks with executable memory and cache invalidation on `IC IVAU`/`__clear_cache`, and it will be slow.
- **Euclidier (`sd88me/mpc-vst-euclidier`): out of scope for in-process emulation.** The `.so` uses `posix_spawn` to
  start a separate static `euclidier` executable (RtMidi, `-lasound`), drives it over an `AF_UNIX` control socket, and
  routes MIDI through ALSA sequencer ports it opens itself. Hosting it means emulating a Linux process, its sockets and an
  ALSA sequencer: a qemu-user-sized job. It is a better fit for a native desktop build (DESKTOP_VST3.md), or for a host
  that speaks Euclidier's socket protocol itself.

So the first target is self-contained DSP ports like Acid-303. Shared-state and JIT ports come second. Ports that spawn
processes come last, or never.

## Prototype: Acid-303 runs emulated (2026-11-06, offline, x86-64 Linux)

`poc/mpc_host/` (`build.sh` fetches dynarmic at a pinned commit, then builds `mpc_host`). It is about 600 lines:

- **Guest memory** (`guest.cpp`) is one 4 GB host mapping, used as dynarmic's fastmem.
- **Loader:** a small ELF32 loader handles the `RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/`ABS32` relocations and runs the
  constructors.
- **Imports:** each import gets a `svc #n; bx lr` thunk that runs a host function. About 150 libc, libm and C++ runtime
  functions are covered. The armhf ABI is followed: floats and doubles go in VFP registers, varargs in core registers
  and on the stack. `printf` formats are re-parsed against the guest's varargs. `operator new`/`delete` use a small
  guest heap.
- **Not covered:** the C++ exception functions (`__cxa_begin_catch`, `__gxx_personality_v0` and others) stop with a
  message if they are called. Acid-303 never calls them.
- **Driver** (`main.cpp`): calls `VSTPluginMain` with a host callback thunk, then `effOpen`, the rate and block size,
  `effMainsChanged` and `effProcessEvents`. It runs `processReplacing` in the 32-bit AEffect layout (processReplacing is
  at byte 80). It renders a 16th-note acid line at 120 BPM, and does the same with a native x86 build of the same port.

```sh
poc/mpc_host/build.sh
poc/mpc_host/build/mpc_host <armv7 acid303.so> [x86 acid303.so] [seconds] [wav prefix]
MPC_HOST_UNSAFE=1 ...        # dynarmic's unsafe float optimisations
```

Results, Acid-303 1.0.2 source, 10 s, 128-frame blocks at 44.1 kHz (a 2902 µs budget), on a 4-core Xeon VM at 2.1 GHz:

| Build | Median per block | p99 | Share of the budget (median) |
|---|---|---|---|
| native x86 (`-O3`, SSE path) | 107-128 µs | 234-284 µs | 3.7-4.4% |
| armv7 under dynarmic | 454-478 µs | 981-1062 µs | 15.6-16.5% |
| armv7 under dynarmic, `MPC_HOST_UNSAFE=1` | 378 µs | 905-946 µs | 13.0% |

- **Output:** the emulated and native renders differ by 91 dB less than the signal (max 3e-5). The voice takes its NEON path on ARM
  and SSE on x86, so the result is not bit-exact. The emulated run needed nothing beyond A32 + NEON (no interpreter
  fallback).
- **Speed:** the emulated build is about 3.5-4.2 times slower than native, which is fine for one instance of a heavy
  voice like this.
- **Spikes:** single blocks reached 2-18 ms after warm-up, when the JIT compiles code it has not seen yet. A real host
  should pre-render at load time to warm the JIT up, and render one block ahead.

Found while building it: armv7 code uses `LDREX`/`STREX` (the C++ runtime's atomics), so dynarmic needs an
`ExclusiveMonitor` even with one CPU. Not done yet: threads, files (`fopen` returns NULL), aarch64 (Gen2) `.so` files, and Windows/macOS builds of the prototype.

## Params and state

The driver calls `setParameter`/`getParameter` (AEffect bytes 12 and 16, floats in `s0`/`r0`-`r1` as the armhf ABI says) and
`effGetChunk`/`effSetChunk` (the chunk pointer is a guest pointer the driver copies out of and into guest memory). After the render it
finds a parameter that reads back what was set (readouts do not), saves the chunk, changes the parameter, restores the chunk and
expects the first value; the exit code is 1 if that fails. `poc/mpc_host/statetest/` is a small engine fixture for it (two parameters
and a `state` string). Checked 2026-11-06, offline: the emulated armv7 build and a native x86 build of the fixture both print
`2/2 parameters restored from a 18-byte chunk (ok)`. The fixture's `sscanf` found a missing import, so the host now
thunks `sscanf` (`%d %i %u %x %o %f %g %s %c %n %%`, `l`, `*`; field widths). The songpos and uiprobe probes have no settable
parameter and report that. Not checked: a real port's chunk (Acid-303 and the others live in their own repos), threads.

## Survey of local builds (2026-11-06, offline)

`mpc_host <armv7 .so> - 1` on the 32-bit ARM `VSTPluginMain` builds in the author's `~/mpc-vst-*` folders (34 files, some of them copies of the same plugin; aarch64
files are not loadable yet). Results:

| Result | Plugins |
|---|---|
| Runs; parameter and chunk round-trip ok | trcw78 (330 B chunk), model303 and acid303 (301 B), boris (361 B, built from source by `smoke.sh`) |
| Runs; chunk restore wrong | morpho_pe (315 params, 407 B chunk): after `effSetChunk` the parameter reads 1.000, not 0.250. Not yet known if the host or the port is at fault: needs a native x86 build to compare |
| Load error `unsupported relocation type 17` (`R_ARM_TLS_DTPMOD32`: `thread_local` data) | rhythmbank, machinedrum (3), cratedigger (2), maze combo and sequencer, model72, monomodule_fx, MM-303, the Dexed arm32 release |
| Stops on a missing import | jv880 (`access`), dx7 (`std::ios_base::Init`), reverbicon224 and OTT (`steady_clock::now`, `wcslen`), clementine (`rand`, `dladdr`), the 8 airwin2rack plugins (`rand`), jn106 (`__aeabi_idiv`), profit5 (`ldexpf`), lucky dip (`std::condition_variable`), plugin manager (`pthread_cond_init`) |

The missing imports, over all of them, are mostly not libc: about 70 are libstdc++ (`std::string`, `std::thread`, `std::condition_variable`,
`_Rb_tree_*`, the `__throw_*` family, C++ exceptions), plus libgcc (`__aeabi_idivmod` 16 files, `__aeabi_ldivmod`, `__aeabi_unwind_cpp_pr0/1`,
`__cxa_end_cleanup`) and libc stdio, dirent and pthread calls (`fclose` 15, `fseek` 13, `rand` 10, `opendir`/`readdir` 7). Thunking all of
these by hand is the wrong shape. The better route is to load the device's armhf `libstdc++.so.6`, `libgcc_s.so.1` and `libm` through the same
ELF loader and keep thunks only for libc and syscalls. That also gives exceptions and `std::thread` for free once `pthread` and `futex` exist.
TLS relocations need a per-guest TLS block and `__aeabi_read_tp`.

## Guest libraries, TLS (2026-11-06, offline)

The loader now takes several modules, each at its own base (64 KB aligned from `0x100000`). An import resolves first to a symbol exported
by a module loaded earlier, then to the host's libc thunks. `MPC_HOST_LIBS=libgcc_s.so.1:libstdc++.so.6` loads armhf libraries before
the plugin; the test used the armhf cross sysroot's `/usr/arm-linux-gnueabihf/lib` (glibc 2.43 era), not a device's libraries. `libm.so.6` does not load
(IFUNC relocations): the host's own libm thunks are used. TLS: one block per module at the thread pointer (`0x000C0000`, ARM variant 1),
`R_ARM_TLS_DTPMOD32/DTPOFF32/TPOFF32`, `__tls_get_addr`, `__aeabi_read_tp`; one thread only. `pthread_once` is a few ARM instructions
in guest memory (so the init routine runs without a nested call from a thunk, which is still unsupported). Also added: a "C"-locale-only
set of locale and wide-character thunks, pthread keys, mutex/condition no-ops, `vsnprintf`, `errno`, and failing file and directory calls.
`MPC_HOST_LAX=1` logs an unsupported import once and returns 0, to list what a plugin really calls.

Re-run of the survey with `MPC_HOST_LIBS` set (same 34 files; the check now sets up to 8 parameters, saves the chunk, changes them,
restores, and compares each with its own saved value):

| Result | Plugins |
|---|---|
| Runs; all checked parameters restored from the chunk | Dexed DX7 (2289 B), jn106, profit5, trcw78 (2 files), model303 and acid303, morpho_pe, clementine and clementine_xt, maze combo and sequencer, the 8 airwin2rack plugins, and, with threads (below), lucky dip (374 B), model72 (869 B), monomodule_fx (313 B). OTT, both Reverbicon builds and jv880 have settable parameters and no chunk |
| Runs; restore not confirmed | machinedrum_one restores 0/8 from a 512 B chunk, also with two audio blocks processed after the restore. The armv7 build is older than the source (it writes `v1|...`, the source writes `v2|`; the current builds are aarch64 only), and 511 characters look cut off for 525 slots: the cause is in that old build, not found in the host. machinedrum tap and tapfx have no settable parameter |
| Stops | MM-303: `getcwd`, sockets (device IPC). cratedigger (2 files): `VSTPluginMain` returns no `AEffect`; it imports `posix_spawn` and threads, so it probably starts helper processes. rhythmbank: imports the ALSA sequencer (`snd_seq_open`); the `abort` follows a failed open |

A first run of this table showed clementine and morpho_pe restoring every parameter to 1.000. That was the host: `sscanf` ignored field
widths, so a hex chunk read with `%2x` parsed as one number. Widths are implemented now and both restore correctly. The lesson for the next
failure: a state bug that hits every parameter is probably a libc thunk, not the port.

C++ exceptions (2026-11-06, emulated): on ARM the unwinder finds its tables through `__gnu_Unwind_Find_exidx(pc, &count)`, not
`dl_iterate_phdr`, so the host answers it from each module's `PT_ARM_EXIDX` segment and no guest-code trampoline is needed.
`poc/mpc_host/cxxtest/` throws two frames deep and catches (with `std::string` and `std::map`); `smoke.sh` runs it on the cross sysroot's
armhf libgcc_s and libstdc++ (CI installs `g++-arm-linux-gnueabihf`).

## Threads (2026-11-06, offline)

Guest threads are cooperative on the one JIT. A switch happens inside a thunk by swapping the register file, so a thread that blocks
resumes at the `bx lr` after its own `svc`; no nested `Run()` is needed. Rules:
- `pthread_create` runs the new thread first, until it blocks or exits; the parent then gets 0 back. Each thread has a 512 KB stack.
- Blocking calls are `pthread_mutex_lock` on a held mutex (glibc armhf layout: count +4, owner +8, kind +12; recursive kind 1),
  `pthread_cond_wait`/`timedwait`/`clockwait`, `pthread_join`, `usleep`/`nanosleep`/`clock_nanosleep`/`sleep`, and `poll` with a timeout.
  `sched_yield` switches to the next ready thread.
- Threads run only inside a host call. When it returns, `drain()` runs each ready worker until it blocks again, with the host call's
  registers put back afterwards. A sleeping worker wakes on the next host call. While the main thread sleeps or waits inside a call, the
  clock advances so sleepers become ready; a main-thread wait nobody can end returns as a timeout, or as a spurious wakeup for an untimed
  one (allowed by POSIX), and a main-thread wait on a mutex held by a parked thread is reported as a deadlock.
- TLS is one block per module, shared by all guest threads (only one runs at a time, so this is safe unless a thread is switched out inside code
  that holds thread-local state). pthread keys are shared too. A worker that computes for a long time without blocking holds the audio call.
- Fixture: `poc/mpc_host/threadtest/` routes every parameter write through a worker (mutex and condition variable); `smoke.sh` and CI run it.
  Parameter 0 reads back one set late there: the wrapper holds parameter 0 until the next call (`pend0`), and the worker only runs after a call.
  Checked 2026-11-06, emulated: lucky dip, model72, monomodule_fx and the fixture pass the state check; jv880 loads and sets parameters (its ROMs are not installed).
- `MPC_HOST_TRACE=1` prints every thunk call with the guest thread id. That showed a polling worker (`opendir`, `nanosleep`) running forever
  until the clock was only advanced while the main thread waits.

## Files, qsort, JV-880 (2026-11-06, offline)

- **File access.** `MPC_HOST_ROOT=<dir>` is the guest's `/`: `/sdcard/x` is `<dir>/sdcard/x`. `/plugin/` is the directory of the plugin `.so`, and `dladdr` reports
  `/plugin/<name>`. Without `MPC_HOST_ROOT` only `/plugin/` exists. Paths with `..` are refused. Done: `fopen`/`fclose`/`fread`/`fwrite`/`fgets`/`fgetc`/`fputs`/`fseek`/`ftell`
  (and the 64-bit forms), `fprintf`/`fscanf` on files, `open`/`read`/`write`/`lseek64`/`close` on host fds (ARM `O_*` flags translated), `stat` in the old `__xstat`
  family (88-byte `struct stat`, 104-byte `stat64`) and the glibc 2.34+ `__stat64_time64` (112 bytes), `opendir`/`readdir`/`readdir64`, `glob`, `mkdir`, `access`, `remove`, `rename`,
  `getcwd` (always `/`). `poc/mpc_host/filetest/` checks them; `smoke.sh` copies the fixture's `*.txt` next to the `.so` and uses a fresh scratch root.
  ROM sets are not copied or committed: link them into a root (`ln -s ~/roms/jv880 $ROOT/sdcard/vst/jv880-roms/roms`).
- **qsort and bsearch** take a guest comparator, which a host thunk cannot call. `guest_helpers.c` provides them as armhf guest code (a stable merge sort); `build.sh`
  builds `build/guest_helpers.so` when `arm-linux-gnueabihf-gcc` is installed and `mpc_host` loads it before any other module.
- **A bug found on the way:** the first `put_stat` cleared 128 bytes, but the old-ABI `struct stat` is 88, so it wrote over the guest's stack and lucky dip, which had
  passed with `stat` failing, jumped to address 0 once `stat` succeeded. It clears exactly the size of each flavour now. `MPC_HOST_TRACE=1` also prints each module's address range.
- **JV-880** (`mpc-vst-jv880`, the ROM set from `~/roms/jv880`, linked as above): loads the ROMs and 19 expansions (4133 patches in 22 banks) on a background thread, starts its emulation
  thread, and renders sound: rms 0.041, peak 0.229 over 4 s of the test line, median 1.98 ms per 128-frame block (68% of the 2.9 ms budget, p99 3.5 ms, max 3.7 ms) on
  the 4-core VM, one instance. The state check restores 8/8 parameters from an 8162-byte chunk.
- Not working: **cratedigger** returns no `AEffect` even with `/media/*/AddOns/ForceCrateDigger` created (it is a device add-on that runs `yt-dlp` and a shell);
  **rhythmbank** needs the ALSA sequencer; **clementine** (the non-XT build) needs `iswspace`; **machinedrum_one** is unresolved (see the survey).

## aarch64 (Gen2) guests (2026-11-06, offline)

`mpc_host` runs aarch64 plugins on dynarmic's A64 front end (`build.sh` builds both front ends). The driver reads the plugin's ELF class and builds `Guest(false)`
(armhf) or `Guest(true)` (aarch64); there is no other switch. What differs from the armhf guest:
- **Memory.** The guest still has a 4 GB arena (dynarmic fastmem, 32 address bits) but every address is below 2 GB (heap `0x10000000-0x70000000`, stack top `0x7FF00000`),
  so a 32-bit value is a whole pointer and a sign-extended `int` return is also a correct `long` or pointer return. Pointers stored in guest memory are 8 bytes (`wrp`/`rdp`).
- **Loader.** `load64`: ELF64, RELA relocations (`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS64`), TLS variant 1 with a 16-byte TCB at `TPIDR_EL0 = 0x000C0000`
  (`TLS_DTPMOD/DTPREL/TPREL`, `TLSDESC` with a two-instruction guest resolver, `__tls_get_addr`). `DT_RELR` is refused (not seen in any local build).
- **Calls and threads.** AAPCS64 (x0-x7, v0-v7, 8-byte stack slots, 16-byte aligned sp); thread contexts hold x0-x30, sp, pc, v0-v31, PSTATE, FPCR, FPSR; `svc #n; ret` thunks (the
  return stub uses `svc #0xFFFF`, since A64 `svc` has 16 bits).
- **Varargs.** `printf`/`scanf` read integer arguments from x(n), doubles from v0-v7, then 8-byte stack slots; `v*printf` read the aarch64 `va_list` structure
  (`stack`, `gr_top`, `vr_top`, `gr_offs`, `vr_offs`). `%ld`, `%zu`, `%jd` are 64-bit.
- **Layouts.** `long`/`time_t` 64-bit (`time`, `gettimeofday`, `clock_gettime`), aarch64 `struct stat` (128 bytes), only the 64-bit `dirent`, pointer-sized `glob_t`, `Dl_info`,
  `pthread_t`, `pthread_mutex_t` (48 bytes, kind at +16), `tls_index`.
- **Unwinding.** The aarch64 unwinder asks `_dl_find_object(pc, &result)` for the module's `.eh_frame_hdr`; the host answers from `PT_GNU_EH_FRAME`.
  Not done: `dl_iterate_phdr` (older libgcc), which needs a guest-code callback loop.
- **Instructions dynarmic's A64 front end lacks.** Newer compilers emit LSE atomics (`SWP`, `LDADD`/`LDCLR`/`LDEOR`/`LDSET`/`LDSMAX`/`LDSMIN`/`LDUMAX`/`LDUMIN`, `CAS`)
  and `LDAPR`. They arrive in `InterpreterFallback` and are emulated there as plain loads and stores (one guest CPU, one thread at a time), then execution continues. Other missing
  instructions still stop with the instruction word printed.
- **Helpers.** `guest_helpers64.so` (qsort, bsearch) is built when `aarch64-linux-gnu-gcc` (or `$A64_CC`) is installed. `ARCH=aarch64 poc/mpc_host/smoke.sh <fixture>/vst.json`
  builds a fixture for aarch64 (cross compiler `$A64_CC`, libraries from `$A64_LIBDIR`, default `/usr/aarch64-linux-gnu/lib`) and runs it; CI runs statetest, threadtest, filetest and cxxtest this way.

Checked 2026-11-06 on the 57 aarch64 `VSTPluginMain` builds found in the author's `~/mpc-vst-*` folders (libgcc_s and libstdc++ from the Ubuntu arm64 cross packages, loaded as guest libraries):
- **All checked parameters restored from the chunk (30 files):** model303, acid303, Dexed DX7, JV-880 (ROM set linked into `MPC_HOST_ROOT`, 8162 B), maze voice and sequencer, boris, clementine_xt, lucky dip, morpho_pe,
  profit5, trcw78, kr106, monomodule fx and one, the 8 airwin2rack plugins, and others. Audio rms is non-zero for the synths (model303 0.013, Dexed 0.042, maze voice 0.101, JV-880 0.041).
  Speed on the 4-core VM, 128-frame blocks (2.9 ms): model303 16% of the budget, maze voice 5%, boris 2%, Dexed under 1%.
- **Settable parameters, no chunk:** OTT, Reverbicon 224 (cineol224).
- **Partial restores, looked at with `MPC_HOST_DUMP=1` (the chunk text before and after the restore); none points at the host:**
  - je8086, vavra, nodalred2x, xenia, ms2kr: the chunk is a small JSON or text record (bank, preset, gain and a few fields, 53-258 B), not every knob, so parameters outside
    it keep the value the test set. xenia's and ms2kr's chunks come back identical after the restore. vavra and nodalred2x read the bank list from files: with no bank files under
    `MPC_HOST_ROOT` the bank clamps to what exists (bank 15 comes back as 2 and 0, and a set of 0.25 reads back 0.238).
  - osirus: the restore is ignored (the chunk after still holds the test's values, `rom_model` is `"?"`): it very likely needs its ROM, which is not under the root.
  - machinedrum_one/b: the chunk is exactly 512 bytes for 672 slots, so it is cut off, most likely by a 512-byte chunk buffer in the wrapper copy those builds were made with
    (mpc-vst-plugins now has 8192). Not confirmed against that checkout.
- **Stops on one import:** welle (`readlink`), the plugin manager (`statvfs`). **cratedigger** returns no `AEffect` (it needs its engine bundle, see above).

## Next steps

1. Run the same survey on every catalog release (`catalog/plugins/*.json` repos, armv7 and aarch64 zips), from a session
   that can reach those repos, and list the union of imports and syscalls.
2. Done for Acid-303 (see "Prototype"). Params and state chunks and guest libraries: done 2026-11-06 (offline, x86 Linux): see the two sections above. Threads: done (see "Threads"). Next: `dl_iterate_phdr` for older libgcc, a real-time scheduling plan for the emulation threads (JV-880 uses 68% of a block), the build on Windows (MSVC or mingw) and macOS
   (arm64 host: dynarmic's arm64 back end).
3. Then the helper process with shared-memory transport, the VST3 shell, the Windows/macOS builds and the editor windows.

## Repo move (done 2026-11-06)

This repo was split out of mpc-vst-plugins with `git filter-repo`, keeping the history of `wrapper/vst3_*`, `wrapper/resample.h`,
`tools/build_vst3.sh`, `tools/test_vst3.sh`, `tools/vst3_host_test.c`, `tools/desktop_skin.py`, `docs/DESKTOP_VST3.md` (and its
images), this file and `poc/mpc_host/`. mpc-vst-plugins is pinned as a submodule, and the scripts read it from there, or from
`MPC_VST_PLUGINS`. Checked offline the same day: `tools/test_vst3.sh mpc-vst-plugins/poc/songpos/vst.json` printed "all passed",
and `poc/mpc_host/smoke.sh` ran the songpos probe emulated. CI (`.github/workflows/ci.yml`) runs both, against the pinned commit
and against mpc-vst-plugins main.

Left to do in mpc-vst-plugins (its own PR): stub `tools/build_vst3.sh`/`test_vst3.sh` that point here; pointer pages for
`docs/DESKTOP_VST3.md` and `docs/MPC_HOST.md`; README, PORTING.md, skill step 8, CLAUDE.md, ROADMAP.md and COMPATIBILITY.md
links; tag `pre-desktop-split`; a Discord note.

# Handoff: the hosted VST3 ("OpenMPC Host"), state at 2026-11-08

Read [MPC_VST3_HOST.md](MPC_VST3_HOST.md) (design, decisions, per-milestone status with dates) and [MPC_HOST.md](MPC_HOST.md) (the emulator) first. This page is what to do next and what bit us.

## What exists and was verified (Windows 11 x64, native, scripted clicks; Linux for the module/audio tests; never in a real DAW)

- `poc/mpc_host/`: the emulator (dynarmic, A32 + A64, ELF loader, libc thunks, threads, files), `HostedPlugin` (per-instance), CLI with `--check`, Windows build and package. Guest faults (`GuestFault`) stop only the plugin.
- `vst3host/`: one VST3 module `OpenMPC Host.vst3`: runtime parameters, Q-Link section (4 or 8 knobs, Bank, Next Bank, Mode, Page; maps from the skin's `Q-Links.json`), plugin switching inside one instance, state, MIDI/CC/pitch bend mapping, bypass; Win32/GDI+ editor (Skin tab at the skin's native size, Plugin tab, native Manager tab); native catalog/installer; a simulated MPC (`devicesim`) so the **unchanged** Plugin Manager discovers, downloads, installs and "restarts".
- Package: `vst3host/package_windows.sh` -> `dist/OpenMPC-Host-vst3-<version>-windows-x64.zip` (module, guest libs, the released Plugin Manager, README, `install.bat`/`uninstall.bat`). Last built: `8e00872`.
- Tests that exist: `tools/vst3_host_test.c` against the module (Linux), `vst3host/switch_test.c` (plugin switching), `vst3host/view_test.c` (Windows: opens the editor, scripted clicks, BMP screenshots; modes: default, `skin`, `mgr`, `mgr2` = manager install end to end), `poc/mpc_host/smoke.sh` x 4 fixtures x 2 arches, `poc/mpc_host/faulttest` (a plugin that faults: build it by hand, see `docs/MPC_VST3_HOST.md`; not wired into a script yet).

## To do, in the order I would do it

1. **Try it in Ableton Live** (the owner's DAW; Windows, Live 11/12): scan, load on a MIDI track, play Acid / Dexed, open the editor, switch plugin from the editor (the window resizes through `IPlugFrame::resizeView`), save and reload a set, map a Q-Link to a controller. Things most likely to need a fix: how Live treats the 2000+ hidden MIDI-mapping parameters, the editor size on a small or high-DPI screen (fixed pixels; `canResize` is false), scan time (the manager loads during `initialize`), state size. The log is `%APPDATA%\OpenMPC Host\root\openmpc-host.log`.
2. **Scaling**: `IPlugViewContentScaleSupport` + draw with a scale (GDI+ `ScaleTransform`, GDI text sizes, hit mapping `skin_click`), and shrink-to-fit on small screens; then `canResize`/`checkSizeConstraint`.
3. **Skin view gaps** (`editor_win.inc`): popup lists (the skin's `open` params: REGEN-style dropdowns), momentary buttons (press = 1, release = 0; today a tap sends a toggle), mouse wheel, keyboard, page images load synchronously on first draw (about a second).
4. **CI** for `vst3host`: Linux module build + `vst3_host_test` + `switch_test` (needs a PIC dynarmic, see `build_linux.sh`), Windows cross-build + `view_test` under Wine if it runs; add `faulttest`. Not in `.github/workflows/ci.yml` yet.
5. **Linux editor** (X11 + cairo: reuse the skin items from `skin.cpp`; `vst3_editor.c` in mpc-vst-plugins is the model) and, later, macOS (owner said leave it for now).
6. **Simulated device**: Remove/Update through the manager, theme swap, addins, the register flow; only the manager's current command strings are handled (anything else is logged to `<root>/tmp/openmpc-pm/devicesim.log`). If the manager's `apply.sh` format changes, `DeviceSim::apply` is the place.
7. **Quality of hosted plugins**: ~35 of 57 aarch64 builds restore state fully; JV-880 ~50% and Osirus ~115% of real time (blocked by `--check`); cratedigger needs an engine bundle; `__pthread_key_create` weak import (std::thread detection) not done; machinedrum chunk stops at 512 bytes (stale wrapper in the user's repo).
8. **Docs sync** when any of this changes (README, `MPC_VST3_HOST.md` with dates, offline vs DAW). Bump nothing in the submodule without CI against mpc-vst-plugins main.

## How to build and test here (what worked, 2026-11-08)

- No sudo: toolchains were unpacked from Ubuntu `.deb`s into the session scratchpad (boost headers, `aarch64-linux-gnu-gcc`, mingw-w64 posix, wine). On a normal machine install `libboost-dev ninja-build gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf gcc-aarch64-linux-gnu g++-aarch64-linux-gnu g++-mingw-w64-x86-64-posix` and the scripts work as is. Overrides: `BOOST_INCLUDEDIR`, `MINGW_BIN`, `A64_LIBDIR`/`A64_DOC`, `MPC_HOST_BUILD_WIN`, `A64_CC`.
- Order: `poc/mpc_host/build.sh` (native dynarmic + helpers) -> `poc/mpc_host/build_windows.sh` (mingw dynarmic) -> `vst3host/build_windows.sh` / `vst3host/build_linux.sh` -> `vst3host/package_windows.sh`. `guest_helpers*.so` (which hold the libcurl lookalike) must be rebuilt for both arches when `guest_helpers.c` changes and copied to the Windows build dir.
- Windows tests run from WSL through interop: copy the bundle to `C:\Users\<you>\Downloads\...`, pass env vars with `WSLENV=OPENMPC_HOST_ROOT` (use a fresh root folder per run), run `view_test.exe <module> <shot prefix> [mode]`; screenshots are BMPs (`PIL` converts). `view_test` runs an audio thread: **the hosted plugin's own housekeeping (trigger spring-back, prompts, its UI state) only happens inside `process()`**, so a test without audio looks dead.
- `/tmp` filled up (tmpfs, 9.8 GB) with old scratch; keep big installs under the home folder.
- Don't delete the user's Downloads leftovers without asking (the safety check blocks wildcard removals): `pkgtest*`, `pkgroot*`, `simroot*`, `ohv*`.

## Rules that still apply

Never commit the Steinberg SDK, Akai skins, firmware or third-party plugins (the one exception is the OpenMPC Plugin Manager in the Windows package: the owner's MIT project, fetched from the catalog at package time, never committed). macOS: leave alone. One commit per concern; session trailers on commits; docs in the same change.

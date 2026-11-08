# The hosted VST3: released MPC plugins in a desktop DAW (design, 2026-11-06)

`docs/DESKTOP_VST3.md` builds a port's source as a VST3. This document is the other route: a VST3 that **runs an unchanged MPC plugin** (the armv7 or aarch64 `.so`) under the
emulator of `docs/MPC_HOST.md`. Status: **design agreed in outline, library done, module not started.** Nothing here is built except `poc/mpc_host/hosted.{h,cpp}`.

## What it should be (from the request, 2026-11-06)

1. A VST3 with an **interface**: the plugin window shows the hosted plugin's MPC skin, as the source-built VST3 does.
2. The **OpenMPC Plugin Manager is the default** hosted plugin: opening the VST3 with nothing else configured shows the manager.
3. A **Q-Link section** with a choice of **4 Q-Links (MPC style)** or **8 (Force style)**, and a **knob button that cycles the banks**.

## Pieces

| Piece | Where | State |
|---|---|---|
| Emulated plugin as a library (params, names, MIDI, transport, state, automation) | `poc/mpc_host/hosted.{h,cpp}` | done (CLI uses it; fixtures and real plugins unchanged) |
| VST3 module: factory, component + processor + controller in one object, runtime parameter list, MIDI, state, resampling, latency | `vst3host/` (new) | not started |
| Bundle format and packer | `tools/make_hosted_vst3.py` (new) | not started |
| Editor (skin) | reuse `wrapper/vst3_editor.c` ideas; Linux (X11) first, Windows after | not started |
| Q-Link section | module + editor | not started |
| Plugin Manager as default | bundle contents + host services (below) | not started |

### The module

Reuse from `mpc-vst-plugins/wrapper`: `vst3_abi.h` (the hand-written ABI), `resample.h` (host rate to 44100), the conventions of `vst3_wrap.c` (parameter IDs = VST2 index, changes applied at the start of
`process()`, values the plugin changes itself go out through `outputParameterChanges`, hidden parameters turn CCs and pitch bend back into MIDI, state through `IBStream`). What differs: the parameter
list is **read from the hosted plugin at load time** (`effGetParamName` / label / display), not compiled from `params.json`; one mutex protects the emulator (the audio thread holds it per block, state and
UI calls take it); audio runs in 128-frame blocks at 44100 Hz exactly as on the device. Several instances in one DAW are independent (`HostedPlugin` is per instance).

### One module, many bundles

A VST3 has a fixed identity (class ID, name) read when the DAW scans it. So one generic module binary is copied into **a bundle per hosted plugin**; `Contents/Resources/plugin.json` gives the name, vendor,
class ID (derived from the name), category (instrument or effect) and which `.so` to load, and the `.so`, its skin and its data sit next to it. The default bundle is **"OpenMPC Plugin Manager"**.
`tools/make_hosted_vst3.py <plugin zip or folder> --out <dir>` makes a bundle from a released plugin (its zip has the armv7 and aarch64 `.so` and the skin folder).

### The interface

The source-built editor reads its parameter table from the generated `params.h`. The skin file (`desktop.skin`, from `tools/desktop_skin.py`) already binds controls to **VST2 parameter indices**, which is
what the hosted plugin has, so the drawing and the touch rules carry over; what must change is the part that asks `PARAMS[p]` for type, option count and text, which becomes calls on the hosted plugin
(`effGetParamDisplay` for text; option counts from the skin's `btn n=`). A released plugin's skin folder is in its zip (`Plugin Skins/…/TUI.json`), so the bundle packer flattens it with `desktop_skin.py`.
Linux first (X11 + cairo + FreeType, as today); Windows needs a Win32 window and paint layer (`DESKTOP_VST3.md`, "Windows": not written yet).

### Q-Links (4 or 8, bank button)

Facts from the devices (`mpc-vst-plugins/docs/NOTES.md`): a skin page defines **16 Q-Link keys**, a 4 x 4 grid numbered from the bottom row up (the top-left control is Q-Link 13); each page (tab) has its own set;
the MPC One / Live have **4 knobs** and a Q-Link button that moves to the next column; the Force has **8 knobs**, and with `Bank Direction: Column` bank 1 is Q-Links 13, 9, 5, 1, 14, 10, 6, 2 and bank 2 is those plus 2.

The VST3 exposes this as parameters (so a DAW can automate them and map a hardware controller to them) and as knobs in the editor:
- **Q-Link Mode**: 4 (MPC) or 8 (Force). In mode 4 a bank is one column of the grid (4 banks); in mode 8 it is two columns (2 banks).
- **Q-Link 1..8**: the knobs. Knob *n* of the current bank drives the hosted parameter that the **current page's** skin assigns to that Q-Link key.
- **Q-Link Bank** (stepped) and **Q-Link Next** (a momentary button: the knob button, cycling the banks and wrapping).
- **Page**: which skin page's Q-Link set is live (the editor's tab; without an editor, page 1).
Open: the Q-Link key to parameter assignments live in the skin's `TUI.json` (`qlinks` lines and handle remapping); `desktop_skin.py` drops them today, so it needs to write them out.
For a plugin with no skin, the fallback is parameter order: knob *n* of bank *b* is visible parameter `b * mode + n`.

### The Plugin Manager as the default

The manager (`mpc-vst-plugins/tools/plugin_manager_g2`, `openmpc_pm_g2.so`) is written to run **on an MPC** and change it: it reads the catalog with the system `libcurl` (`dlopen("libcurl.so.4")`),
runs `sha256sum` and `unzip` as child processes, writes the plugin folders and `MPC.settings`, and restarts MPC. Inside the emulator today its status says "No libcurl" / "No unzip" (checked 2026-11-06:
`dlopen` and `posix_spawn` fail, as designed), so it browses nothing and installs nothing. To make it useful as the default the host provides **narrow built-in services instead of general process spawning**
(a guest plugin is untrusted code on the user's PC and must not be able to run arbitrary programs):
1. `dlopen("libcurl.so.4")` resolves to a **host-provided curl shim** (`curl_easy_init/setopt/perform/cleanup/strerror`): HTTPS GET only, to `https://` URLs, no redirects to other schemes, size and time limits; the host does the networking (Windows WinHTTP, Linux libcurl if present).
2. `posix_spawn` of **`sha256sum`** and **`unzip`** (and the few shell lines the manager uses) are recognised by name and run as **built-in host implementations** on files inside the sandbox root; any other command fails.
3. The restart step and `MPC.settings` writes land in the sandbox root and do nothing else. After an install the VST3 rescans `<root>/sdcard/Synths` and offers the installed plugins to switch to (a "Plugin" list parameter and, in the editor, a launcher page).

Still to decide (see below): which hosts the curl shim may reach, and whether switching the hosted plugin inside one VST3 instance is wanted, or the user should instead generate a bundle per plugin.

## Decisions (owner, 2026-11-07)

- **One VST3**, "OpenMPC Host": it starts on the Plugin Manager and switches the hosted plugin inside the instance (a plugin picker in the editor and a `Plugin` entry in the state). Per-plugin bundles are optional later.
- **Q-Links default to 4 knobs** (8 selectable); **the Q-Link parameters are visible in the DAW's parameter list** (Q-Link 1..8, Bank, Next Bank, Mode).
- **Network:** the curl shim may reach any `https://` URL (GET only, to a file or memory; no other protocols).
- **Windows first** (`.vst3` DLL via mingw, Win32 editor), then Linux (X11 editor exists), macOS deferred.

## Milestones (Windows first)

1. **M1 core module** (portable C++/C, built as a Windows `.vst3` with mingw and as a Linux `.vst3`): factory, one object as IComponent + IAudioProcessor + IEditController, runtime parameters from `HostedPlugin`, audio (128-frame blocks at 44.1 kHz, resampled), MIDI, state (`Plugin` name + chunk), Q-Link/Bank/Next/Mode parameters. Default plugin = the manager. Test: `tools/vst3_host_test.c` (add a hosted-module switch), then Wine, then the user's Windows DAW.
2. **M2 plugin discovery and switching:** scan `<root>/sdcard/Synths` (and the plugin folder next to the module) for `.so` + skin; switch plugin in an instance; remember it in state.
3. **M3 manager services:** curl shim (`libcurl.so.4` dlopen -> host HTTPS via WinHTTP on Windows / a small socket+TLS-free fallback is not allowed; use WinHTTP, libcurl on Linux), built-in `sha256sum` and `unzip`, rescan after install.
4. **M4 Win32 editor:** the skin in the window (GDI+/Direct2D), live values, touch/mouse, plugin picker, Q-Link strip with bank button. Then the X11 editor from `vst3_editor.c`.
5. **M5 Q-Link sets from skins:** `tools/desktop_skin.py` writes the page's Q-Link set (TUI.json `qlinks`); fallback parameter order `b*mode+n`.
6. **M6 Linux build, packaging, CI** (and docs sync).

## Handoff (state at 2026-11-07)

**Done and committed:** `poc/mpc_host/hosted.{h,cpp}` (`HostedPlugin`, per-instance: `open`, `start`, `num_params`, `param_name/label/display`, `set_param/get_param`, `midi(status,d1,d2,delta)`, `set_transport`, `process(l,r)` in 128-frame blocks, `get_chunk/set_chunk`, `take_automation`, `take_display_dirty`; not thread-safe, so the VST3 holds one mutex). The CLI (`main.cpp`) uses it. Windows build of the CLI works (`build_windows.sh`, `package_windows.sh`, `smoke_windows.sh`; tested under Wine and natively). Design = this file.

**M1 done (2026-11-07, offline):** `vst3host/vst3_host.cpp` is the module (IComponent + IAudioProcessor + IEditController + IMidiMapping in one object; parameters: Q-Link 1..8, Bank, Next Bank, Mode, Bypass, then the hosted plugin's, plus hidden CC / pressure / pitch-bend parameters for the host's MIDI mapping). `vst3host/build_linux.sh` and `build_windows.sh` make `OpenMPC Host.vst3`. It hosts `Resources/plugins/<name>/<name>.so` (`Resources/default.txt` names the folder, default `openmpc_pm_g2`; `OPENMPC_HOST_PLUGIN=<path to .so>` overrides). The plugin's "/" is `%APPDATA%/OpenMPC Host/root` (`~/.local/share/openmpc-host/root`; `OPENMPC_HOST_ROOT`). `tools/vst3_host_test.c` passes on Linux and natively on Windows 11: the manager loads (309 parameters, no sound by nature), Dexed (aarch64) sounds at 44.1/48/96 kHz with the same level on both, state round-trips. Not yet tried in a DAW. Q-Links use the fallback map (knob n of bank b = parameter b*mode+n); no editor yet; switching plugins inside the instance (M2) not yet.

**M2 done (2026-11-07, offline, Linux; the Windows build compiles, not yet run):** the `Plugin` list parameter (id 13) switches the hosted plugin inside the instance on a worker thread (`process()` is silent meanwhile, then `restartComponent(values|titles)`); plugins come from `Resources/plugins/<name>/*.so` and `<root>/sdcard/Synths/<name>/*.so`; the state keeps the choice. `vst3host/switch_test.c` (manager -> Dexed) passes.

**M3 finding (2026-11-07):** the unchanged manager cannot install anything on a desktop even with curl, `sha256sum` and `unzip` provided. Its device probe needs `systemctl` and `systemd-run` (else "No systemctl"), it reads `MPC.settings` and the device's content locations, and an install is a restart of MPC followed by `apply.sh` and each package's `install.sh` (shell scripts, `awk`, `grep`, `cut`, `/sys/fs/cgroup`). Faking a device around that is fragile. Recommended instead: a **host-native manager service** in the module (catalog.json from open-mpc.github.io, sha256, zip unpack with a small inflate, install into `<root>/sdcard/Synths/<name>/`, rescan, switch), shown in the editor; the unchanged manager stays loadable as a plugin but is not the install path. Needs the owner's OK (see the handoff message).

**M3 service done (2026-11-07, offline plus the live catalog, Linux):** `vst3host/installer.{h,cpp}` is the host-native manager: catalog.json (open-mpc.github.io), download (libcurl on Linux, WinHTTP on Windows), sha256, unzip with its own inflate, install of the package's `portable/<folder>` into `<root>/sdcard/Synths/<folder>/` (files the user added stay). `vst3host/pm_cli.cpp` drives it: `pm_cli list [filter]`, `pm_cli install <id> <root>`; installing the real Plugin Manager G2 0.2.5 from GitHub worked (sha256 verified, 2487 files, the .so is there). Not yet: wiring into the module and the editor's manager panel (M4); the Windows build of pm_cli is compiled, the run is pending.

**M4a done (2026-11-07, offline, native Windows 11):** a Win32/GDI editor (`vst3host/editor_win.inc`, 720x420, IPlugView `HWND`): a Plugin tab (the plugins on offer, click to switch; the Q-Link section: 4 or 8 knobs you drag, Mode, Bank < >, Next bank; knob labels and values are the hosted plugin's own parameters) and a Manager tab (Refresh reads the live catalog, 207 plugins; Install downloads, verifies, installs and switches to it). `vst3host/view_test.c` opens it in a window and saves screenshots; the Plugin Manager's real Q-Links (View, Filter, Sort, Dropdown) show. Install from the editor was driven too (native Windows, 2026-11-08): Manager tab, Refresh, select Acid, Install -> "Installed Acid 2.1.0", it appears in the Plugin list and the instance switches to it (Generate / Mutate / Density / Accent). A plugin that fails to load leaves the previous one running and shows the error (also in `<root>/openmpc-host.log`). Not yet: the hosted plugin's skin in the editor (M4b), keyboard, DPI scaling, the Linux editor.

**M5 done (2026-11-08, native Windows + Linux host test):** the knobs follow the hosted plugin's skin: `<plugin folder>/Plugin Skins/Q-Links.json` (`Q-Links - 8by1.json` when loaded in 8-knob mode) maps each page (Tab/SubTab) and Q-Link number (1..16, bottom row first) to a parameter index. With `Bank Direction: Column`, 4 knobs = 4 banks, one grid column each (bank 1 = Q-Links 13, 9, 5, 1), 8 knobs = 2 banks (13, 9, 5, 1, 14, 10, 6, 2, then +2); `Row` pages go by rows. A `Q-Link Page` parameter and Page buttons pick the skin page (Acid: 6 pages, page 1 shows Density, Accent, Slide, Octaves, as the skin maps them). Plugins without a skin use the old order. The 8by1 map is reloaded when the mode changes (and on state restore), and pages carry the skin's tab names (TUI.json `tabName`, matched by `fnKeyIndex`/`fnKeySubIndex`): Acid shows SEQ A, EDIT A, SEQ B, EDIT B, MOD, GLOBAL; in 8-knob mode page 2 shows Step 1..8 (verified natively on Windows 2026-11-08).

**M4b done (2026-11-08, native Windows):** the editor has a Skin tab (960x560, the default for a plugin that has a skin) that draws the hosted plugin's own skin: `vst3host/skin.{h,cpp}` is `tools/desktop_skin.py` in C++ (verified item-for-item against the Python converter on Acid, all 6 pages), drawn with GDI+ (images, film-strip knobs, on/off buttons, labels with live `effGetParamDisplay` values, IndexedEnabling conditions) from a snapshot so the emulator is not held while drawing. Taps and drags work: radio/toggle buttons and knob drags set the parameter (and tell the host); the skin's tab strip also selects the matching Q-Link page. Verified by scripted clicks: DIR -> REV, Density dragged 70 -> 99, page EDIT A (step sequencer). Not yet: popup lists (`open` flags), momentary buttons, mouse wheel, resizing, the first paint of a page loads its PNGs (about a second), the Linux editor.

**Simulated device for the stock Plugin Manager (2026-11-08, native Windows, scripted clicks):** the unchanged manager now runs as designed on the Skin tab. `vst3host/devicesim.{h,cpp}` gives it a stand-in MPC under the plugin's root: `data/Settings/MPC/MPC.settings` (Gen2 layout, rewritten from the plugin folders), `nvme/Synths` as the install location, a `/proc/mounts` with `/nvme` as exFAT, stub `systemctl`/`systemd-run`/`unzip`/`sha256sum` files so its tool checks pass, and `statvfs`. The guest side (poc/mpc_host): `Guest::Hooks` lends HTTP GET (a libcurl lookalike in `guest_helpers.c`, found by `dlopen("libcurl.so.4")`, with the write and progress callbacks run as guest code) and `posix_spawn(p)`/`waitpid`; the host runs the manager's few fixed shell commands natively (sha256 check, `unzip -p/-l/-q`, the zip-listing checks) and reads its `apply.sh` for the `item ... install/remove` lines, doing what `install.sh`/`uninstall.sh` do (copy `portable/<folder>`, `%payload-path%`, the entry in MPC.settings, the state file, the result file), then the "restart" reloads the hosted plugin and rescans. New libc thunks: strtoll/strtoull, strncasecmp, strncat, strpbrk, fsync, localtime_r (UTC), the socket calls fail cleanly. Verified: ONLINE, 34.4 GB free, catalog with INSTALL buttons, INSTALL Keyscope -> APPLY -> "Restart MPC?" prompt (Install to /nvme/Synths) -> RESTART & APPLY -> the plugin is in `nvme/Synths/jacob-sabella - VST - Keyscope`, the manager restarts and says "Last apply: 1 done." Needs audio to be running (a DAW always runs it): the plugin's housekeeping (trigger spring-back, prompts) happens inside `process()`, and the editor polls the visible page's parameters back from the plugin. The native Manager tab stays as the quick path. Not yet: Remove/Update through the manager, theme swap, the manager's register/addin flows, and the host-side command set only covers the manager's current command strings (unknown commands are logged to `<root>/tmp/openmpc-pm/devicesim.log`).

**A hosted plugin can no longer take the DAW down (2026-11-08):** every fatal guest event (an unsupported libc call, a CPU fault, `abort`, a failed assert, a call to address 0, a deadlock) used to `abort()` the process. Now `Guest::Impl::fatal` records the reason and halts the CPU, `Guest::call` throws `GuestFault` once the CPU is out of the JIT, `HostedPlugin` catches it (`dead()`, `fault()`; later calls return zeros, `process()` is silent), the module shows "The plugin stopped: ..." in the Plugin tab and picking the same plugin again restarts it. `poc/mpc_host/faulttest` is a fixture that calls an unsupported import after 20 blocks; the VST3 test host runs through it without dying. (The CLI still ends with an uncaught-exception message, as it did with `abort`.)

**Ableton Live (the owner's DAW):** `vst3host/package/install.bat` / `uninstall.bat` (system VST3 folder, asks for admin), README steps (Live 11/12, "Use VST3 System Folders", Rescan, Configure). Open points for Live, not testable here: window scaling on high-DPI screens (the editor is a fixed 960x560 in device pixels), how Live behaves with the 2000+ hidden MIDI-mapping parameters (JUCE plugins publish the same), scanning time while the manager loads.

**Not started:** M6 (Linux editor, CI).

**Where M1 began (done):**
1. Create `vst3host/` with `vst3_host.cpp` (hand-written ABI: reuse `mpc-vst-plugins/wrapper/vst3_abi.h` and `resample.h`; model the factory and object layout on `mpc-vst-plugins/wrapper/vst3_wrap.c`), linking `guest.cpp hosted.cpp` and dynarmic from `poc/mpc_host/build*/` (see `build.sh` / `build_windows.sh` for flags).
2. `vst3host/build_windows.sh` -> `OpenMPC Host.vst3/Contents/x86_64-win/*.vst3` (+ guest runtime libs as in `package_windows.sh`).
3. Test with a small host (extend `tools/vst3_host_test.c`), then Wine, then the user's Windows 11 PC (WSL interop works: see the earlier Windows test flow; copy to `C:\Users\Sam\Downloads`).
4. Parameter list is read at load from the hosted plugin; plugin changes must signal `restartComponent(kParamTitlesChanged)`.

**Constraints to keep:** never commit plugin binaries/ROMs/Steinberg SDK; macOS left alone; skip mm-303; commit per concern with the session trailers; docs sync (this file + `docs/MPC_HOST.md`, dated, offline vs DAW). Known limits: ~35 of 57 aarch64 builds restore state fully; JV-880 ~50% of real time (degraded), Osirus ~115% (blocked) on this VM; `--check` verdicts gate which plugins load; the manager needs the M3 services before it can browse or install (it reports "No libcurl" / "No unzip" today).

**Test material:** user's plugins and ROMs in `~/roms` and local folders; fixtures under `poc/mpc_host/{statetest,threadtest,filetest,cxxtest}`; `poc/mpc_host/smoke.sh` (ARCH=armv7|aarch64).

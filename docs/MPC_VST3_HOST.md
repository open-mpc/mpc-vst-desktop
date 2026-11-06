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

## Milestones

1. **M1 core module**, Linux: factory, runtime parameters, audio, MIDI, state; generic DAW parameter view; default bundle contents = the manager. Test with `tools/vst3_host_test.c` (needs a small switch for hosted bundles).
2. **M2 packer**: `make_hosted_vst3.py` from a released plugin zip.
3. **M3 editor**, Linux: the skin in the window, live values, touch.
4. **M4 Q-Links**: parameters and knobs, 4 / 8 modes, bank button; `desktop_skin.py` writes the Q-Link sets.
5. **M5 manager services**: curl shim, `sha256sum` / `unzip` built-ins, rescan and switch.
6. **M6 Windows**: the module as a `.vst3` DLL (mingw, as for the source-built VST3), then a Win32 editor.

## Questions for the owner

- **Switching inside one VST3, or one bundle per plugin?** The first suits the manager-as-default idea (install, then pick); the second matches how DAWs name and save plugins. Both can exist (a "launcher" bundle plus per-plugin bundles).
- **Q-Link default:** 4 or 8? And should the Q-Link parameters be visible in the DAW's list or only through the editor and a mapped controller?
- **Network:** may the curl shim reach only the catalog host (`open-mpc.github.io`) and GitHub release downloads, or any `https://` URL?
- **Platforms first:** Linux (X11 editor exists) or Windows (no editor yet)?

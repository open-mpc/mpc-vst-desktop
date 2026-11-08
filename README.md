# mpc-vst-desktop

MPC OS plugins on a desktop DAW. Two methods, one repo:

| | How | Status |
|---|---|---|
| **Rebuild as VST3** | Build a port from [mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins) source as a VST3, with its MPC skin as the plugin window | Linux: loads in Reaper (2026-10-30). Windows: cross-built, tested under Wine only. See [docs/DESKTOP_VST3.md](docs/DESKTOP_VST3.md) |
| **Host released plugins** | Run the released armv7/aarch64 `.so` unchanged, under an ARM JIT (dynarmic) inside a VST3 | Emulator: armv7 and aarch64 guests, Linux and Windows, `--check` verdicts ([docs/MPC_HOST.md](docs/MPC_HOST.md)) |
| **OpenMPC Host (the hosted VST3)** | One VST3 that hosts one MPC plugin at a time and switches between them: the plugin's own skin at native size, Q-Link section (4 or 8 knobs, banks, skin pages), the stock Plugin Manager running on a simulated MPC | Windows x64: tested natively with scripted clicks (2026-11-08), not yet in a DAW (Ableton Live next). Design [docs/MPC_VST3_HOST.md](docs/MPC_VST3_HOST.md), handoff [docs/HANDOFF_VST3_HOST.md](docs/HANDOFF_VST3_HOST.md) |

Goal: Windows and macOS first-class for both.

## Setup

```sh
git clone --recurse-submodules https://github.com/open-mpc/mpc-vst-desktop
```

`mpc-vst-plugins/` is a pinned submodule: the engine contract, `gen_vst.py`, the skin builder and the probe ports. To use
another checkout, set `MPC_VST_PLUGINS=<path>`.

## Rebuild a port as a VST3

```sh
tools/build_vst3.sh <port>/vst.json [--install]   # <port>/build/vst3/<name>.vst3
tools/test_vst3.sh <port>/vst.json                # offline test under ASan/UBSan
```

Build the port's skin first with mpc-vst-plugins' `tools/build_port.sh` to get the skin editor.

## Host prototype

```sh
poc/mpc_host/build.sh                              # fetches dynarmic (pinned), builds poc/mpc_host/build/mpc_host
poc/mpc_host/build/mpc_host <armv7 .so> [native .so] [seconds] [wav prefix]
poc/mpc_host/smoke.sh                              # builds a probe for armv7 and runs it emulated
```

## OpenMPC Host (VST3)

```sh
poc/mpc_host/build.sh && poc/mpc_host/build_windows.sh   # emulator, native and mingw (needs the packages listed in docs/HANDOFF_VST3_HOST.md)
vst3host/build_windows.sh                                # vst3host/build-win/OpenMPC Host.vst3
vst3host/package_windows.sh                              # dist/OpenMPC-Host-vst3-<version>-windows-x64.zip (install.bat inside)
vst3host/build_linux.sh                                  # Linux module (no editor yet), tested with tools/vst3_host_test.c
```

This repo never ships plugins, firmware, Akai's skins or the Steinberg SDK. The ABIs are hand-written.

## History

Moved from mpc-vst-plugins on 2026-11-06, with history. The desktop VST3 wrapper is by Jacob Sabella.

## Licence

MIT (see LICENSE). dynarmic, fetched at build time, is 0BSD.

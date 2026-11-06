# mpc-vst-desktop

MPC OS plugins on a desktop DAW. Two methods, one repo:

| | How | Status |
|---|---|---|
| **Rebuild as VST3** | Build a port from [mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins) source as a VST3, with its MPC skin as the plugin window | Linux: loads in Reaper (2026-10-30). Windows: cross-built, tested under Wine only. See [docs/DESKTOP_VST3.md](docs/DESKTOP_VST3.md) |
| **Host released plugins** | Run the released armv7/aarch64 `.so` unchanged, under an ARM JIT inside a VST3, with a library browser like MPC's | Prototype: Acid-303's armv7 build plays on x86-64 Linux at about 4x native CPU (2026-11-06). See [docs/MPC_HOST.md](docs/MPC_HOST.md) |

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

This repo never ships plugins, firmware, Akai's skins or the Steinberg SDK. The ABIs are hand-written.

## History

Moved from mpc-vst-plugins on 2026-11-06, with history. The desktop VST3 wrapper is by Jacob Sabella.

## Licence

MIT (see LICENSE). dynarmic, fetched at build time, is 0BSD.

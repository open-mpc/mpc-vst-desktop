# mpc-vst-desktop: agent guide

MPC OS plugins (VST2 for Akai MPC/Force standalone devices) on a desktop DAW:

1. **Rebuild from source as a VST3** (`tools/build_vst3.sh`, `wrapper/vst3_wrap.c`, skin editor `wrapper/vst3_editor.c`):
   `docs/DESKTOP_VST3.md`.
2. **Host the released armv7/aarch64 `.so` unchanged** under an ARM JIT (dynarmic) with an ELF loader and libc thunks:
   `docs/MPC_HOST.md`, prototype in `poc/mpc_host/`.
3. **OpenMPC Host**: one VST3 hosting the released plugins (skin editor, Q-Links, Plugin Manager on a simulated MPC), `vst3host/`:
   `docs/MPC_VST3_HOST.md`; what is left and how to build/test: `docs/HANDOFF_VST3_HOST.md`.

The ports, the engine contract (`engine.h`), `gen_vst.py` (`params.h`), the skin builder, adapters and device facts live in
[mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins), pinned here as the `mpc-vst-plugins/` submodule
(`git submodule update --init`; or `MPC_VST_PLUGINS=<checkout>`). Read its `docs/NOTES.md` for verified device behaviour.

Ground rules:
- Never commit the Steinberg SDK (the VST2/VST3 ABIs here are hand-written), Akai's stock skins, firmware, or third-party
  plugin binaries. The host loads what the user installed; it never bundles plugins (many are GPL/AGPL or need the user's
  own firmware).
- dynarmic is fetched at a pinned commit by `poc/mpc_host/build.sh`, never committed.
- Offline first: `tools/test_vst3.sh <port>/vst.json` (ASan) and `poc/mpc_host/smoke.sh` before a DAW or device.
- Docs sync: a change to a feature, tool or verified fact updates its doc in the same PR, with dates and offline vs
  DAW/device. Bump the submodule in its own commit, after CI passes against mpc-vst-plugins main.
- Commit trailers per the session's instructions; one commit per concern.

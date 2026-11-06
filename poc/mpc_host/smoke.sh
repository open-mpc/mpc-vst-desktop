#!/usr/bin/env bash
# Smoke test of the desktop host prototype: builds mpc_host, builds an mpc-vst-plugins probe port for armv7 with the
# armhf cross compiler (ARCH=aarch64: for aarch64 with $A64_CC, default aarch64-linux-gnu-gcc), and runs it emulated.
#   poc/mpc_host/smoke.sh [port vst.json, default: the submodule's poc/songpos; poc/mpc_host/statetest/vst.json also checks parameters and state chunks]
# Needs what build.sh needs, plus arm-linux-gnueabihf-gcc (or the aarch64 compiler).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/../.." && pwd)
MV=$(cd "${MPC_VST_PLUGINS:-$HERE/mpc-vst-plugins}" && pwd)
CFG=${1:-$MV/poc/songpos/vst.json}
OUT=${MPC_HOST_BUILD:-$HERE/poc/mpc_host/build}
"$HERE/poc/mpc_host/build.sh"
# C++ fixtures run on the armhf libgcc_s and libstdc++ of the cross sysroot (the host loads them as guest libraries)
ARCH=${ARCH:-armv7}
if [ "$ARCH" = aarch64 ]; then CC=${A64_CC:-aarch64-linux-gnu-gcc}; SR=${A64_LIBDIR:-/usr/aarch64-linux-gnu/lib}; ARCH_FLAGS=
else CC=arm-linux-gnueabihf-gcc; SR=/usr/arm-linux-gnueabihf/lib; ARCH_FLAGS=; fi
case $CFG in *cxxtest*) export MPC_HOST_LIBS=${MPC_HOST_LIBS:-$SR/libgcc_s.so.1:$SR/libstdc++.so.6} ;; esac
eval "$(python3 "$MV/tools/gen_vst.py" "$CFG" --shell)"   # SO, SOURCES, CFLAGS, LIBS, ROOT, PORT
python3 "$MV/tools/gen_vst.py" "$CFG" --params-h >/dev/null
SO_OUT="$OUT/$ARCH/$SO"
mkdir -p "$OUT/$ARCH"
(cd "$ROOT" && $CC -O2 -fPIC -shared -fvisibility=hidden -std=gnu11 $CFLAGS $([ "$ARCH" = armv7 ] && echo $CFLAGS_ARM) -I"$PORT/build" -I"$MV/wrapper" \
    $SOURCES "$MV/wrapper/vst2_wrap.c" $LIBS -lpthread -lm -Wl,--no-undefined -o "$SO_OUT")
# data files next to the plugin (/plugin/ in the guest) and a scratch root for writes (MPC_HOST_ROOT)
for f in "$ROOT/$PORT"/*.txt; do [ -f "$f" ] && cp "$f" "$OUT/$ARCH/"; done
export MPC_HOST_ROOT="$OUT/root"; rm -rf "$MPC_HOST_ROOT"; mkdir -p "$MPC_HOST_ROOT"
"$OUT/mpc_host" "$SO_OUT" - 2 "$OUT/smoke" | tee "$OUT/smoke.txt"
grep -q '^emulated: ' "$OUT/smoke.txt" && grep -q 'median' "$OUT/smoke.txt"
# The statetest fixture has settable parameters and a chunk: the host must round-trip both.
case $CFG in *statetest*|*threadtest*|*cxxtest*|*filetest*) grep -q 'parameters restored from .*(ok)' "$OUT/smoke.txt" ;; esac
echo "smoke: passed"

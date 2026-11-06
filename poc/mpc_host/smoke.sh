#!/usr/bin/env bash
# Smoke test of the desktop host prototype: builds mpc_host, builds an mpc-vst-plugins probe port for armv7 with the
# armhf cross compiler, and runs it emulated.
#   poc/mpc_host/smoke.sh [port vst.json, default: the submodule's poc/songpos; poc/mpc_host/statetest/vst.json also checks parameters and state chunks]
# Needs what build.sh needs, plus arm-linux-gnueabihf-gcc.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/../.." && pwd)
MV=$(cd "${MPC_VST_PLUGINS:-$HERE/mpc-vst-plugins}" && pwd)
CFG=${1:-$MV/poc/songpos/vst.json}
OUT=${MPC_HOST_BUILD:-$HERE/poc/mpc_host/build}
"$HERE/poc/mpc_host/build.sh"
eval "$(python3 "$MV/tools/gen_vst.py" "$CFG" --shell)"   # SO, SOURCES, CFLAGS, LIBS, ROOT, PORT
python3 "$MV/tools/gen_vst.py" "$CFG" --params-h >/dev/null
SO_OUT="$OUT/armv7/$SO"
mkdir -p "$OUT/armv7"
(cd "$ROOT" && arm-linux-gnueabihf-gcc -O2 -fPIC -shared -fvisibility=hidden -std=gnu11 $CFLAGS $CFLAGS_ARM -I"$PORT/build" -I"$MV/wrapper" \
    $SOURCES "$MV/wrapper/vst2_wrap.c" $LIBS -lpthread -lm -Wl,--no-undefined -o "$SO_OUT")
"$OUT/mpc_host" "$SO_OUT" - 2 "$OUT/smoke" | tee "$OUT/smoke.txt"
grep -q '^emulated: ' "$OUT/smoke.txt" && grep -q 'median' "$OUT/smoke.txt"
# The statetest fixture has settable parameters and a chunk: the host must round-trip both.
case $CFG in *statetest*|*threadtest*) grep -q 'parameters restored from .*(ok)' "$OUT/smoke.txt" ;; esac
echo "smoke: passed"

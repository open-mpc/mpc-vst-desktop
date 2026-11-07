#!/usr/bin/env bash
# Cross-builds "OpenMPC Host.vst3" for 64-bit Windows with mingw-w64 (docs/MPC_VST3_HOST.md). Uses the dynarmic tree poc/mpc_host/build_windows.sh builds.
#   vst3host/build_windows.sh        -> vst3host/build-win/OpenMPC Host.vst3
#   MPC_HOST_BUILD_WIN=<dir>  the build_windows.sh output directory (default poc/mpc_host/build-win); MINGW_BIN, BOOST_INCLUDEDIR as there
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); MH=$HERE/../poc/mpc_host
WB=${MPC_HOST_BUILD_WIN:-$MH/build-win}; DYN=${DYNARMIC_DIR:-$MH/build/dynarmic}
[ -n "${MINGW_BIN:-}" ] && export PATH=$MINGW_BIN:$PATH
CXX=x86_64-w64-mingw32-g++-posix
[ -f "$WB/dynarmic/src/dynarmic/libdynarmic.a" ] || { echo "run poc/mpc_host/build_windows.sh first" >&2; exit 1; }
BUNDLE=$HERE/build-win/OpenMPC\ Host.vst3
mkdir -p "$BUNDLE/Contents/x86_64-win" "$BUNDLE/Contents/Resources/plugins"
LIBS=$(find "$WB/dynarmic" -name '*.a' | grep -v libdynarmic.a | tr '\n' ' ')
$CXX -std=c++20 -O2 -g -Wall -Wno-reorder -shared -static -Wl,--kill-at -I"$DYN/src" -I"$DYN/externals/mcl/include" -I"${BOOST_INCLUDEDIR:-/usr/include}" \
    "$HERE/vst3_host.cpp" "$HERE/installer.cpp" "$HERE/skin.cpp" "$MH/guest.cpp" "$MH/hosted.cpp" "$WB/dynarmic/src/dynarmic/libdynarmic.a" $LIBS $LIBS -lwinhttp -lgdiplus -lole32 -lgdi32 -luser32 -o "$BUNDLE/Contents/x86_64-win/OpenMPC Host.vst3"
for h in guest_helpers.so guest_helpers64.so; do [ -f "$WB/$h" ] && cp "$WB/$h" "$BUNDLE/Contents/Resources/"; done
echo "built $BUNDLE"

#!/usr/bin/env bash
# Builds "OpenMPC Host.vst3" for x86-64 Linux (docs/MPC_VST3_HOST.md). Needs poc/mpc_host/build.sh to have fetched dynarmic.
#   vst3host/build_linux.sh [plugin folder with <name>/<name>.so]   -> vst3host/build/OpenMPC Host.vst3
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); MH=$HERE/../poc/mpc_host
DYN=${DYNARMIC_DIR:-$MH/build/dynarmic}; B=$MH/build/dynarmic-pic; OUT=$HERE/build; BUNDLE="$OUT/OpenMPC Host.vst3"
# a shared module needs position-independent code: a second dynarmic build from the same pinned source
if [ ! -f "$B/src/dynarmic/libdynarmic.a" ]; then
  cmake -S "$DYN" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DBoost_INCLUDE_DIR="${BOOST_INCLUDEDIR:-/usr/include}" -DCMAKE_CXX_FLAGS="-I${BOOST_INCLUDEDIR:-/usr/include}" \
        -DDYNARMIC_WARNINGS_AS_ERRORS=OFF "-DDYNARMIC_FRONTENDS=A32;A64" >/dev/null
  ninja -C "$B" >/dev/null
fi
mkdir -p "$BUNDLE/Contents/x86_64-linux" "$BUNDLE/Contents/Resources/plugins"
LIBS=$(find "$B" -name '*.a' | grep -v libdynarmic.a | tr '\n' ' ')
g++ -std=c++20 -O2 -g -Wall -Wno-reorder -shared -fPIC -fvisibility=hidden -I"$DYN/src" -I"$DYN/externals/mcl/include" "$HERE/vst3_host.cpp" "$HERE/installer.cpp" "$HERE/skin.cpp" "$HERE/devicesim.cpp" "$MH/guest.cpp" "$MH/hosted.cpp" \
    "$B/src/dynarmic/libdynarmic.a" $LIBS $LIBS -ldl -lpthread -o "$BUNDLE/Contents/x86_64-linux/OpenMPC Host.so"
for h in guest_helpers.so guest_helpers64.so; do [ -f "$MH/build/$h" ] && cp "$MH/build/$h" "$BUNDLE/Contents/Resources/"; done
echo "built $BUNDLE"

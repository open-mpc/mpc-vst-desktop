#!/usr/bin/env bash
# Cross-builds the desktop-host prototype for 64-bit Windows with mingw-w64 (docs/MPC_HOST.md): dynarmic (both front ends) and mpc_host.exe, static.
#   poc/mpc_host/build_windows.sh        -> poc/mpc_host/build-win/mpc_host.exe
# Needs: what build.sh needs (git, cmake, ninja, libboost-dev) plus g++-mingw-w64-x86-64-posix (the POSIX threading flavour: dynarmic uses std::thread).
#   MINGW_BIN=<dir>   a directory with the x86_64-w64-mingw32-*-posix tools when they are not on PATH
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${MPC_HOST_BUILD_WIN:-$HERE/build-win}
DYN_REPO=https://github.com/azahar-emu/dynarmic
DYN_REV=a46601580d5512d324104f985b5f0209dc980ddc
DYN=${DYNARMIC_DIR:-$HERE/build/dynarmic}   # the same pinned source tree build.sh fetches
[ -n "${MINGW_BIN:-}" ] && export PATH=$MINGW_BIN:$PATH
CC=x86_64-w64-mingw32-gcc-posix; CXX=x86_64-w64-mingw32-g++-posix
command -v "$CXX" >/dev/null || { echo "no $CXX: install g++-mingw-w64-x86-64-posix or set MINGW_BIN" >&2; exit 1; }
mkdir -p "$OUT"
if [ ! -d "$DYN/.git" ]; then
  mkdir -p "$(dirname "$DYN")"; git clone -q "$DYN_REPO" "$DYN"; git -C "$DYN" checkout -q "$DYN_REV"
fi
git -C "$DYN" submodule update -q --init --depth 1 externals/fmt externals/mcl externals/xbyak externals/zydis externals/zycore externals/robin-map
cat > "$OUT/toolchain.cmake" <<TC
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $(command -v $CC))
set(CMAKE_CXX_COMPILER $(command -v $CXX))
set(CMAKE_RC_COMPILER $(command -v x86_64-w64-mingw32-windres))
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_EXE_LINKER_FLAGS "-static")
TC
if [ ! -f "$OUT/dynarmic/src/dynarmic/libdynarmic.a" ]; then
  cmake -S "$DYN" -B "$OUT/dynarmic" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$OUT/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release -DDYNARMIC_TESTS=OFF \
        -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF "-DDYNARMIC_FRONTENDS=A32;A64" \
        -DBoost_INCLUDE_DIR="${BOOST_INCLUDEDIR:-/usr/include}" -DCMAKE_CXX_FLAGS="-I${BOOST_INCLUDEDIR:-/usr/include}" >/dev/null
  ninja -C "$OUT/dynarmic" >/dev/null
fi
LIBS=$(find "$OUT/dynarmic" -name '*.a' | grep -v libdynarmic.a | tr '\n' ' ')
VERSION=$(git -C "$HERE" describe --always --dirty 2>/dev/null || echo dev)
"$CXX" -std=c++20 -O2 -g -Wall -DMPC_HOST_VERSION="\"$VERSION\"" -I"$DYN/src" -I"$DYN/externals/mcl/include" -I"${BOOST_INCLUDEDIR:-/usr/include}" "$HERE/guest.cpp" "$HERE/main.cpp" \
    "$OUT/dynarmic/src/dynarmic/libdynarmic.a" $LIBS $LIBS -static -o "$OUT/mpc_host.exe"
# the guest helper libraries are armhf / aarch64 ELF files, the same on every host: reuse the ones build.sh made
for h in guest_helpers.so guest_helpers64.so; do [ -f "$HERE/build/$h" ] && cp "$HERE/build/$h" "$OUT/$h"; done
echo "built $OUT/mpc_host.exe"

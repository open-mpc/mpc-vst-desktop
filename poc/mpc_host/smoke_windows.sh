#!/usr/bin/env bash
# Runs the fixtures through the Windows build (build_windows.sh) under Wine, for both guest architectures, and requires the same results as smoke.sh:
# parameters and chunks round-trip, threads, files, C++ exceptions, and a "ok" verdict from --check.
#   poc/mpc_host/smoke_windows.sh
# Needs build_windows.sh's mpc_host.exe, the fixtures smoke.sh built (build/armv7, build/aarch64: run `ARCH=armv7 smoke.sh` and `ARCH=aarch64 smoke.sh` for statetest, threadtest,
# filetest and cxxtest first), and Wine. WINE=<command> picks the Wine launcher (default wine64); A64_LIBDIR / ARM_LIBDIR point at the guest libstdc++ directories.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
EXE=${MPC_HOST_BUILD_WIN:-$HERE/build-win}/mpc_host.exe
B=${MPC_HOST_BUILD:-$HERE/build}
WINE=${WINE:-wine64}
ARM_LIBDIR=${ARM_LIBDIR:-/usr/arm-linux-gnueabihf/lib}; A64_LIBDIR=${A64_LIBDIR:-/usr/aarch64-linux-gnu/lib}
export MPC_HOST_ROOT=$(mktemp -d)
fail=0
for arch in armv7 aarch64; do
  lib=$ARM_LIBDIR; [ $arch = aarch64 ] && lib=$A64_LIBDIR
  for t in statetest threadtest filetest cxxtest; do
    so=$B/$arch/$t.so
    [ -f "$so" ] || { echo "missing $so: run ARCH=$arch smoke.sh first" >&2; exit 2; }
    export MPC_HOST_LIBS=; [ $t = cxxtest ] && export MPC_HOST_LIBS="$lib/libgcc_s.so.1;$lib/libstdc++.so.6"   # ';' separates paths on Windows
    out=$("$WINE" "$EXE" "$so" - 1 "$MPC_HOST_ROOT/x" 2>&1 | tr -d '\r') || true
    if echo "$out" | grep -q 'parameters restored from .*(ok)'; then ok=state; else ok=; fi
    chk=$("$WINE" "$EXE" --check "$so" 120 2>&1 | tr -d '\r' | grep '^{' || true)
    if [ -n "$ok" ] && echo "$chk" | grep -q '"verdict":"ok"'; then echo "windows $arch $t: ok"; else echo "windows $arch $t: FAILED"; echo "$out" | tail -3; echo "$chk"; fail=1; fi
  done
done
rm -rf "$MPC_HOST_ROOT"
[ $fail = 0 ] && echo "smoke_windows: passed"
exit $fail

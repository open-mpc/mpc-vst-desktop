#!/usr/bin/env bash
# Builds a downloadable Windows package of the desktop host: dist/mpc_host-<version>-windows-x64.zip
#   poc/mpc_host/package_windows.sh
# Needs: build_windows.sh's mpc_host.exe, the fixtures smoke.sh built (build/armv7, build/aarch64: statetest, threadtest, filetest, cxxtest), the armhf and aarch64 libgcc_s/libstdc++ guest libraries
# (ARM_LIBDIR, A64_LIBDIR) with their copyright files (ARM_DOC, A64_DOC), and the mingw strip tool (MINGW_BIN).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd)
OUT=${MPC_HOST_BUILD_WIN:-$HERE/build-win}; B=${MPC_HOST_BUILD:-$HERE/build}
ARM_LIBDIR=${ARM_LIBDIR:-/usr/arm-linux-gnueabihf/lib}; A64_LIBDIR=${A64_LIBDIR:-/usr/aarch64-linux-gnu/lib}
ARM_DOC=${ARM_DOC:-/usr/share/doc/libstdc++6-armhf-cross}; A64_DOC=${A64_DOC:-/usr/share/doc/libstdc++6-arm64-cross}
[ -n "${MINGW_BIN:-}" ] && export PATH=$MINGW_BIN:$PATH
VERSION=$("$OUT/mpc_host.exe" --version 2>/dev/null | tr -d '\r' | awk '{print $2}' || true); : "${VERSION:=$(git -C "$REPO" describe --always --dirty)}"
NAME=mpc_host-$VERSION-windows-x64; STAGE=$(mktemp -d)/$NAME; mkdir -p "$STAGE/guestlibs/armv7" "$STAGE/guestlibs/aarch64" "$STAGE/fixtures/armv7" "$STAGE/fixtures/aarch64" "$STAGE/licenses" "$STAGE/data"
x86_64-w64-mingw32-strip -o "$STAGE/mpc_host.exe" "$OUT/mpc_host.exe"
cp "$OUT/guest_helpers.so" "$OUT/guest_helpers64.so" "$STAGE/"
cp -L "$ARM_LIBDIR/libgcc_s.so.1" "$ARM_LIBDIR/libstdc++.so.6" "$STAGE/guestlibs/armv7/"
cp -L "$A64_LIBDIR/libgcc_s.so.1" "$A64_LIBDIR/libstdc++.so.6" "$STAGE/guestlibs/aarch64/"
for a in armv7 aarch64; do for t in statetest threadtest filetest cxxtest; do cp "$B/$a/$t.so" "$STAGE/fixtures/$a/"; done; cp "$B/$a/seed.txt" "$STAGE/fixtures/$a/"; done
cp "$REPO/LICENSE" "$STAGE/licenses/mpc-vst-desktop-LICENSE.txt"
cp "$ARM_DOC/copyright" "$STAGE/licenses/guestlibs-armv7-copyright.txt"; cp "$A64_DOC/copyright" "$STAGE/licenses/guestlibs-aarch64-copyright.txt"
cp "$HERE/package/README.txt" "$HERE/package/THIRD_PARTY.txt" "$STAGE/"
for f in test.bat check.bat render.bat; do cp "$HERE/package/$f" "$STAGE/$f"; done
python3 - "$STAGE" "$VERSION" <<'PY'
import sys,os,glob
stage,ver=sys.argv[1:3]
for f in ['README.txt','THIRD_PARTY.txt','test.bat','check.bat','render.bat']:   # Windows line endings for the text files
    p=os.path.join(stage,f); d=open(p,newline='').read().replace('@VERSION@',ver).replace('\r\n','\n').replace('\n','\r\n'); open(p,'w',newline='').write(d)
PY
mkdir -p "$REPO/dist"; rm -f "$REPO/dist/$NAME.zip"
(cd "$(dirname "$STAGE")" && python3 -c "
import zipfile,os,sys
n=sys.argv[1]
with zipfile.ZipFile(sys.argv[2],'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for r,_,fs in os.walk(n):
        for f in sorted(fs): p=os.path.join(r,f); z.write(p,p)
    z.writestr(n+'/data/PUT_PLUGIN_DATA_HERE.txt','Files a plugin reads or writes (ROMs, banks, settings) go under here: data\\\\sdcard\\\\... is what a plugin sees as /sdcard/... (see README.txt).\r\n')
" "$NAME" "$REPO/dist/$NAME.zip")
echo "built $REPO/dist/$NAME.zip ($(du -h "$REPO/dist/$NAME.zip" | cut -f1))"

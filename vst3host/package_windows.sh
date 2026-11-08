#!/usr/bin/env bash
# Builds a downloadable Windows package of the VST3: dist/OpenMPC-Host-vst3-<version>-windows-x64.zip
#   vst3host/package_windows.sh
# Needs: build_windows.sh's output, the guest runtime libraries and their copyright files (ARM_LIBDIR, A64_LIBDIR, ARM_DOC, A64_DOC; as poc/mpc_host/package_windows.sh),
# the mingw strip tool (MINGW_BIN), g++ and network access (the Plugin Manager is fetched from the catalog with pm_cli, so the package carries the released build).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/.." && pwd); MH=$REPO/poc/mpc_host
ARM_LIBDIR=${ARM_LIBDIR:-/usr/arm-linux-gnueabihf/lib}; A64_LIBDIR=${A64_LIBDIR:-/usr/aarch64-linux-gnu/lib}
ARM_DOC=${ARM_DOC:-/usr/share/doc/libstdc++6-armhf-cross}; A64_DOC=${A64_DOC:-/usr/share/doc/libstdc++6-arm64-cross}
[ -n "${MINGW_BIN:-}" ] && export PATH=$MINGW_BIN:$PATH
SRC=$HERE/build-win/OpenMPC\ Host.vst3
[ -f "$SRC/Contents/x86_64-win/OpenMPC Host.vst3" ] || { echo "run vst3host/build_windows.sh first" >&2; exit 1; }
VERSION=$(git -C "$REPO" describe --always --dirty | tr -d '\r')
NAME=OpenMPC-Host-vst3-$VERSION-windows-x64; TOP=$(mktemp -d)/$NAME; STAGE=$TOP/OpenMPC\ Host.vst3
mkdir -p "$STAGE/Contents/x86_64-win" "$STAGE/Contents/Resources/guestlibs/armv7" "$STAGE/Contents/Resources/guestlibs/aarch64" "$STAGE/Contents/Resources/plugins" "$TOP/licenses"
x86_64-w64-mingw32-strip -o "$STAGE/Contents/x86_64-win/OpenMPC Host.vst3" "$SRC/Contents/x86_64-win/OpenMPC Host.vst3"
cp "$SRC/Contents/Resources/guest_helpers.so" "$SRC/Contents/Resources/guest_helpers64.so" "$STAGE/Contents/Resources/"
cp -L "$ARM_LIBDIR/libgcc_s.so.1" "$ARM_LIBDIR/libstdc++.so.6" "$STAGE/Contents/Resources/guestlibs/armv7/"
cp -L "$A64_LIBDIR/libgcc_s.so.1" "$A64_LIBDIR/libstdc++.so.6" "$STAGE/Contents/Resources/guestlibs/aarch64/"
# the Plugin Manager the host opens on: the released aarch64 build from the catalog (MIT)
if [ -n "${PM_DIR:-}" ]; then PM=$PM_DIR; PMNAME=$(basename "$PM")   # a local copy of the released manager folder (when the download is unreachable)
else
W=$(mktemp -d); g++ -std=c++20 -O1 "$HERE/installer.cpp" "$HERE/pm_cli.cpp" -ldl -o "$W/pm_cli"
"$W/pm_cli" install plugin-manager-g2 "$W/root"
PM=$(ls -d "$W/root/nvme/Synths/"*/ | head -n 1); PMNAME=$(basename "$PM")
fi
cp -r "$PM" "$STAGE/Contents/Resources/plugins/$PMNAME"
printf '%s\n' "$PMNAME" > "$STAGE/Contents/Resources/default.txt"
cp "$REPO/LICENSE" "$TOP/licenses/mpc-vst-desktop-LICENSE.txt"
cp "$ARM_DOC/copyright" "$TOP/licenses/guestlibs-armv7-copyright.txt"; cp "$A64_DOC/copyright" "$TOP/licenses/guestlibs-aarch64-copyright.txt"
cp "$HERE/package/README.txt" "$HERE/package/THIRD_PARTY.txt" "$HERE/package/install.bat" "$HERE/package/uninstall.bat" "$TOP/"
python3 - "$TOP" "$VERSION" <<'PY'
import sys,os
top,ver=sys.argv[1:3]
for f in ['README.txt','THIRD_PARTY.txt','install.bat','uninstall.bat']:   # Windows line endings
    p=os.path.join(top,f); d=open(p,newline='').read().replace('@VERSION@',ver).replace('\r\n','\n').replace('\n','\r\n'); open(p,'w',newline='').write(d)
PY
mkdir -p "$REPO/dist"; rm -f "$REPO/dist/$NAME.zip"
(cd "$(dirname "$TOP")" && python3 -c "
import zipfile,os,sys
n=sys.argv[1]
with zipfile.ZipFile(sys.argv[2],'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for r,_,fs in os.walk(n):
        for f in sorted(fs): p=os.path.join(r,f); z.write(p,p)
" "$NAME" "$REPO/dist/$NAME.zip")
echo "built $REPO/dist/$NAME.zip ($(du -h "$REPO/dist/$NAME.zip" | cut -f1))"

#!/usr/bin/env bash
# Builds the macOS release for the machine it runs on (Apple Silicon -> arm64, Intel -> x86_64):
#   dist/MPC2500-Emulator-<version>-macOS-<arch>.zip   (contains "MPC2500 Emulator.app")  and a .dmg
# Needs: Xcode command-line tools (clang, make, sips, iconutil, hdiutil, codesign) and curl. The skin artwork must already exist in skin/
# (generated on Linux by tools/make_skin.py, or downloaded by the CI workflow). SDL2 is built from source and linked statically.
#   usage: packaging/macos/build.sh [version]
set -euo pipefail
cd "$(dirname "$0")/../.."
VERSION=${1:-dev}; ARCH=$(uname -m); NAME="MPC2500-Emulator-$VERSION-macOS-$ARCH"; DIST=$PWD/dist
export MACOSX_DEPLOYMENT_TARGET=11.0
[ -f skin/skin.bmp ] || { echo "skin/skin.bmp is missing: run python3 tools/make_skin.py skin first"; exit 1; }
PREFIX=$(packaging/build_sdl.sh "mac-$ARCH" | tail -n 1)
make -C emu clean >/dev/null
make -C emu mpc2500emu CC=clang CFLAGS="-O2 -DAPP_VERSION=\\\"$VERSION\\\"" SDL_CF="$("$PREFIX/bin/sdl2-config" --cflags)" SDL_LD="$("$PREFIX/bin/sdl2-config" --static-libs)"
APP="$DIST/MPC2500 Emulator.app"; rm -rf "$APP" "$DIST/$NAME".*; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp emu/mpc2500emu "$APP/Contents/MacOS/mpc2500emu"; strip -x "$APP/Contents/MacOS/mpc2500emu"
cp -r skin "$APP/Contents/Resources/skin"; rm -f "$APP/Contents/Resources/skin/skin_preview.png" "$APP/Contents/Resources/skin/overlay.png"
cp panel_map.txt LICENSE THIRD-PARTY-NOTICES.md "$APP/Contents/Resources/"
sed "s/@VERSION@/$VERSION/g" packaging/macos/Info.plist > "$APP/Contents/Info.plist"
ICONSET=$(mktemp -d)/icon.iconset; mkdir -p "$ICONSET"
for s in 16 32 64 128 256 512; do sips -z $s $s packaging/icon.png --out "$ICONSET/icon_${s}x${s}.png" >/dev/null; done
for s in 16 32 128 256; do cp "$ICONSET/icon_$((s*2))x$((s*2)).png" "$ICONSET/icon_${s}x${s}@2x.png"; done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/icon.icns"
otool -L "$APP/Contents/MacOS/mpc2500emu"
codesign --force --deep --sign - "$APP"            # ad-hoc signature (required on Apple Silicon); not notarised, see the README for the first-launch steps
mkdir -p "$DIST"
(cd "$DIST" && ditto -c -k --keepParent "MPC2500 Emulator.app" "$NAME.zip")
hdiutil create -volname "MPC2500 Emulator" -srcfolder "$APP" -ov -format UDZO "$DIST/$NAME.dmg" >/dev/null
echo "wrote $DIST/$NAME.zip and $DIST/$NAME.dmg"

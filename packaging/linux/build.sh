#!/usr/bin/env bash
# Builds the Linux release files:  dist/MPC2500-Emulator-<version>-linux-<arch>.tar.gz   (a folder with the program and its resources; double-click "MPC2500 Emulator")
#                                  dist/MPC2500-Emulator-<version>-linux-<arch>.AppImage (only if appimagetool is available: env APPIMAGETOOL=/path/to/appimagetool)
# Needs: gcc, make, curl, python3 + Pillow (for the artwork), and the SDL2 build dependencies (see packaging/README.md). The result needs only libc.
#   usage: packaging/linux/build.sh [version]
set -euo pipefail
cd "$(dirname "$0")/../.."
VERSION=${1:-dev}; ARCH=$(uname -m); NAME="MPC2500-Emulator-$VERSION-linux-$ARCH"; DIST=$PWD/dist
PREFIX=$(packaging/build_sdl.sh "linux-$ARCH" | tail -n 1)
[ -f skin/skin.bmp ] || python3 -I tools/make_skin.py skin
make -C emu clean >/dev/null
make -C emu mpc2500emu CC=gcc CFLAGS="-O2" SDL_CF="$("$PREFIX/bin/sdl2-config" --cflags)" SDL_LD="$("$PREFIX/bin/sdl2-config" --static-libs)" 2>&1 | grep -E "error" || true
strip emu/mpc2500emu
echo "linked libraries:"; ldd emu/mpc2500emu
if ldd emu/mpc2500emu | grep -qE "libSDL2|libX11|libwayland|libpulse|libasound"; then echo "ERROR: SDL or a desktop library is linked dynamically"; exit 1; fi
rm -rf "$DIST/$NAME"; mkdir -p "$DIST/$NAME/resources"
cp emu/mpc2500emu "$DIST/$NAME/MPC2500 Emulator"
cp -r skin "$DIST/$NAME/resources/skin"; rm -f "$DIST/$NAME/resources/skin/skin_preview.png" "$DIST/$NAME/resources/skin/overlay.png"
cp panel_map.txt "$DIST/$NAME/resources/"
cp packaging/icon.png "$DIST/$NAME/icon.png"; cp LICENSE THIRD-PARTY-NOTICES.md "$DIST/$NAME/"
cat > "$DIST/$NAME/README.txt" <<'TXT'
MPC2500 Emulator
================
Double-click "MPC2500 Emulator" to start it (if your file manager asks, choose Run / Execute).
First start: the emulator creates a folder "MPC2500 Emulator" in your home folder. Drag a firmware file onto the window (or copy it into
the folder's "firmware" sub-folder), pick it in the list and press Enter.

No firmware is included - you need your own MPC2500 firmware files.
This software is provided "as is", without warranty of any kind. It is unofficial and not affiliated with Akai Professional.
TXT
mkdir -p "$DIST"; (cd "$DIST" && tar czf "$NAME.tar.gz" "$NAME"); echo "wrote $DIST/$NAME.tar.gz"
# ---- optional AppImage ----
if [ -n "${APPIMAGETOOL:-}" ] && [ -x "$APPIMAGETOOL" ]; then
  APPDIR=$PWD/build/AppDir; rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/mpc2500-emulator"
  cp emu/mpc2500emu "$APPDIR/usr/bin/mpc2500emu"; cp -r "$DIST/$NAME/resources/." "$APPDIR/usr/share/mpc2500-emulator/"
  cp packaging/linux/mpc2500-emulator.desktop LICENSE THIRD-PARTY-NOTICES.md "$APPDIR/"; cp packaging/icon.png "$APPDIR/mpc2500-emulator.png"
  printf '#!/bin/sh\nHERE="$(dirname "$(readlink -f "$0")")"\nexec "$HERE/usr/bin/mpc2500emu" "$@"\n' > "$APPDIR/AppRun"; chmod +x "$APPDIR/AppRun"
  ARCH=$ARCH "$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$DIST/$NAME.AppImage" 2>&1 | tail -3 || ARCH=$ARCH "$APPIMAGETOOL" "$APPDIR" "$DIST/$NAME.AppImage"
  echo "wrote $DIST/$NAME.AppImage"
fi

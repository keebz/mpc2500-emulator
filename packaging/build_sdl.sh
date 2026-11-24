#!/usr/bin/env bash
# Builds a static SDL2 from the official source release into build/sdl-<version>-<tag>; prints the prefix on the last line.
# The Linux build leaves the video/audio back-ends (X11, Wayland, PulseAudio, ALSA, PipeWire) to be loaded at run time, so the finished
# program needs only the C library. usage: packaging/build_sdl.sh <tag>      (env SDL_VER, default 2.30.0)
set -euo pipefail
cd "$(dirname "$0")/.."
TAG=${1:?tag}; SDL_VER=${SDL_VER:-2.30.0}; PREFIX=$PWD/build/sdl-$SDL_VER-$TAG
if [ ! -x "$PREFIX/bin/sdl2-config" ]; then
  mkdir -p build/src; cd build/src
  [ -f "SDL2-$SDL_VER.tar.gz" ] || curl -fsSL -o "SDL2-$SDL_VER.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
  rm -rf "SDL2-$SDL_VER"; tar xf "SDL2-$SDL_VER.tar.gz"; cd "SDL2-$SDL_VER"
  ./configure --prefix="$PREFIX" --disable-shared --enable-static --disable-joystick --disable-haptic --disable-sensor --disable-hidapi --disable-render-d3d >/dev/null
  make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" >/dev/null; make install >/dev/null
fi
echo "$PREFIX"

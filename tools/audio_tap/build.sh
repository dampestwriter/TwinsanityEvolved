#!/bin/sh
# build.sh -- builds the SDL device-side audio tap next to this script (tools/audio_tap/libsdl_audio_tap.{dylib,so}).
#   tools/audio_tap/build.sh [SDL2 prefix]       default: `sdl2-config --prefix`, else /opt/homebrew/opt/sdl2
# Use: SDL_AUDIO_TAP=/abs/out.wav DYLD_INSERT_LIBRARIES=$PWD/tools/audio_tap/libsdl_audio_tap.dylib <program> ...
#      (Linux: LD_PRELOAD=.../libsdl_audio_tap.so)
set -e
here=$(cd "$(dirname "$0")" && pwd)
prefix=${1:-$(sdl2-config --prefix 2>/dev/null || echo /opt/homebrew/opt/sdl2)}
case "$(uname -s)" in
  Darwin) cc -O2 -Wall -shared -fPIC -o "$here/libsdl_audio_tap.dylib" "$here/sdl_audio_tap.c" -L"$prefix/lib" -lSDL2-2.0.0 \
            -Wl,-rpath,"$prefix/lib" && echo "$here/libsdl_audio_tap.dylib" ;;
  *)      cc -O2 -Wall -shared -fPIC -o "$here/libsdl_audio_tap.so" "$here/sdl_audio_tap.c" -ldl && echo "$here/libsdl_audio_tap.so" ;;
esac

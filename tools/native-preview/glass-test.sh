#!/bin/sh
# Builds tools/native-preview/glass-test.c against native/src/render.c and renders
# a Liquid Glass test card with Mesa's GLES2 under Xvfb (Linux, or the
# homeplus-preview Docker image): glass-test.sh <art.jpg> <out.png> [focus] [lx ly]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SDL=$(ls /usr/lib/*/libSDL2-2.0.so.0 /usr/lib/libSDL2-2.0.so.0 2>/dev/null | head -1)
mkdir -p "$ROOT/native/build/host"
cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -I"$ROOT/native/src" -I"$ROOT/native/third_party" -I"$ROOT/native/third_party/SDL2" -D_REENTRANT \
  -o "$ROOT/native/build/host/glass-test" "$HERE/glass-test.c" "$ROOT/native/src/render.c" "$SDL" -lm
xvfb-run -a -s '-screen 0 1920x1080x24' env SDL_VIDEODRIVER=x11 "$ROOT/native/build/host/glass-test" "$@"

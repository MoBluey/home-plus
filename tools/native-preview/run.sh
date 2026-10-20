#!/bin/sh
# Renders native Home+ screenshots into preview/native-*.png using the harness.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
ASSETS=${TMPDIR:-/tmp}/homeplus-native-preview
# the service writes its API token here and the launcher reads it (on the TV: the app folder)
export HOMEPLUS_TOKEN_FILE=${TMPDIR:-/tmp}/homeplus-api-token
python3 "$HERE/make-assets.py" "$ASSETS" "$ROOT/native/assets/fonts/Inter-Bold.ttf" > "$ASSETS.apps" && mv "$ASSETS.apps" "$ASSETS/apps.txt"
make -s -C "$ROOT/native"
NODE_PATH="$HERE/stub" node "$HERE/harness.js" "$ASSETS" "$ROOT/service" & SVC=$!
trap 'kill $SVC 2>/dev/null; rm -rf /var/lib/homeplus/config.json' EXIT
sleep 1
# Prefer a real GLES2 renderer (Mesa under Xvfb) so the glass effects show, like on the TV.
if command -v xvfb-run >/dev/null; then
  RUN="xvfb-run -a -s '-screen 0 1920x1080x24'"; DRIVER="SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=opengles2"
else
  RUN=""; DRIVER="SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software"
fi
shot() { # name keys ms
  eval "$RUN env $DRIVER HOMEPLUS_SCREENSHOT='$ROOT/preview/native-$1.png' HOMEPLUS_KEYS='$2' HOMEPLUS_SCREENSHOT_MS='$3' '$ROOT/native/build/host/homeplus'" 2>/dev/null
  echo "saved preview/native-$1.png"
}
shot home "" 2500
shot stremio "RR" 3000
shot stremio-shelf "RRUR" 3500
shot scrolled "DR" 3000
shot sheet "H" 3500
shot topbar "U" 3000
shot settings "UO" 3500
shot recents "ULLO" 3800

#!/bin/sh
# Builds Home+ and installs it on a rooted webOS TV over SSH.
# usage: tools/install.sh <tv-ip> [--autostart]
#   --autostart  also installs the boot script so the TV starts in Home+
set -eu

TV=${1:?usage: tools/install.sh <tv-ip> [--autostart]}
AUTOSTART=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SSH="ssh -o ConnectTimeout=8 root@$TV"

node "$ROOT/tools/build.mjs"
IPK=$(ls -t "$ROOT"/dist/*.ipk | head -1)

echo "→ copying $(basename "$IPK") to $TV"
scp -o ConnectTimeout=8 "$IPK" "root@$TV:/tmp/homeplus.ipk"

echo "→ installing"
$SSH 'sh -s' <<'REMOTE'
set -e
# Close Home+ and stop the old service so the new code is what runs.
# (Never pkill -f here: this script's own command line would match.)
luna-send -n 1 luna://com.webos.applicationManager/closeByAppId '{"id":"com.atvlauncher.app"}' >/dev/null 2>&1 || true
for p in $(pgrep -f atvlauncher.app.service); do [ "$p" != "$$" ] && kill "$p" 2>/dev/null; done || true
timeout 120 luna-send -i luna://com.webos.appInstallService/dev/install \
  '{"id":"com.ares.defaultName","ipkUrl":"/tmp/homeplus.ipk","subscribe":true}' 2>&1 |
while read -r line; do
  echo "   $line"
  case "$line" in
    *'"state":"installed"'*) exit 0 ;;
    *'"state":"failed"'*|*'"returnValue":false'*) exit 1 ;;
  esac
done
rm -f /tmp/homeplus.ipk

ELEVATE=/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service
if [ -x "$ELEVATE" ]; then
  "$ELEVATE" com.atvlauncher.app.service
else
  echo "!! elevate-service not found — is Homebrew Channel installed and up to date?"
fi
luna-send -n 1 -w 5000 luna://com.atvlauncher.app.service/ping '{}' || true
REMOTE

if [ "$AUTOSTART" = "--autostart" ]; then
  echo "→ installing boot script"
  scp -o ConnectTimeout=8 "$ROOT/init.d/50-homeplus" "root@$TV:/var/lib/webosbrew/init.d/50-homeplus"
  $SSH 'chmod 755 /var/lib/webosbrew/init.d/50-homeplus'
fi

echo "→ launching"
$SSH 'luna-send -n 1 luna://com.webos.applicationManager/launch "{\"id\":\"com.atvlauncher.app\"}"' >/dev/null
echo "done."

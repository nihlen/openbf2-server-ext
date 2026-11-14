#!/usr/bin/env bash
# Extracts the server content from a BF2 server image and turns it into a private OpenBF2 test run directory.
# The image must have the layout of the bf2-docker images (github.com/nihlen/bf2-docker): the server under
# /home/bf2/tmp/srv, with ModManager.
#
# Usage (WSL):  testenv/prepare-content.sh <run-dir> <openbf2-build-dir>
#   run-dir            your own target, e.g. ~/serverext-testenv/srv (recreated from scratch)
#   openbf2-build-dir  the linux-x64 build dir; bin_linux/{bf2,libdice_py.so} replace the retail pair
#
# Environment (all optional):
#   IMAGE        content image            (required: a BF2 server image you may use)
#   GAME_PORT    sv.serverPort            (16571)   the default ports 16567, 29900 and 4711 are refused,
#   GAMESPY_PORT sv.gameSpyPort           (29903)   so a test server never collides with a real one
#   RCON_PORT    mm_rcon.rconPort         (4723)
#   RCON_IP      mm_rcon.rconIp           (127.0.0.1; use 0.0.0.0 inside Docker)
#   RCON_PASSWORD                         (random, printed once)
#   WEBADMIN_HOST/WEBADMIN_PORT  where mm_webadmin connects (127.0.0.1:4303). Keep it away from a real
#                BF2WebAdmin's port so this server never shows up in that admin.
#   SERVER_NAME  sv.serverName            (serverext test)
#
# Deviations from production, all in mods/bf2/settings: sv.internet 0 (no master-server
# registration for a private test server) and sv.punkBuster 0. Everything else - mods, levels,
# ModManager, mm_webadmin, map list starting on dalian_2_v_2 - is the image's content unchanged.
set -euo pipefail

RUN=${1:?usage: prepare-content.sh <run-dir> <openbf2-build-dir>}
BUILD=${2:?usage: prepare-content.sh <run-dir> <openbf2-build-dir>}
IMAGE=${IMAGE:?set IMAGE to a BF2 server content image}
GAME_PORT=${GAME_PORT:-16571}
GAMESPY_PORT=${GAMESPY_PORT:-29903}
RCON_PORT=${RCON_PORT:-4723}
RCON_IP=${RCON_IP:-127.0.0.1}
RCON_PASSWORD=${RCON_PASSWORD:-$(od -An -N9 -tx1 /dev/urandom | tr -d ' \n')}
WEBADMIN_HOST=${WEBADMIN_HOST:-127.0.0.1}
WEBADMIN_PORT=${WEBADMIN_PORT:-4303}
SERVER_NAME=${SERVER_NAME:-serverext test}

for p in "$GAME_PORT" "$GAMESPY_PORT" "$RCON_PORT"; do
    case "$p" in 16567|16569|29900|29901|4711|4721) echo "refusing reserved port $p" >&2; exit 1;; esac
done

rm -rf "$RUN"
mkdir -p "$(dirname "$RUN")"
cid=$(docker create "$IMAGE")
trap 'docker rm "$cid" >/dev/null' EXIT
docker cp "$cid:/home/bf2/tmp/srv" "$RUN"

cd "$RUN"
sub() { sed -i -e "s|$1|$2|g" "$3"; }
S=mods/bf2/settings/serversettings.con
M=mods/bf2/settings/modmanager.con
sub '{{server_name}}'  "$SERVER_NAME"    $S
sub '{{max_players}}'  16                $S
sub '{{server_port}}'  "$GAME_PORT"      $S
sub '{{gamespy_port}}' "$GAMESPY_PORT"   $S
sub '{{demos_url}}'    'http://127.0.0.1/demos/' $S
sed -i -e 's/^sv.internet .*/sv.internet 0/' -e 's/^sv.punkBuster .*/sv.punkBuster 0/' $S
sub '{{rcon_password}}' "$RCON_PASSWORD" $M
sub '{{bf2wa_host}}'    "$WEBADMIN_HOST" $M
sub '{{bf2wa_port}}'    "$WEBADMIN_PORT" $M
sub '{{bf2wa_timer_interval}}' 0.25      $M
sed -i -e "s/^mm_rcon.rconPort .*/mm_rcon.rconPort $RCON_PORT/" \
       -e "s/^mm_rcon.rconIp .*/mm_rcon.rconIp \"$RCON_IP\"/" $M

# Keep the retail binaries for reference; the OpenBF2 pair goes where start.sh looks.
mkdir -p bin/retail-amd-64
mv bin/amd-64/bf2 bin/amd-64/libdice_py.so bin/retail-amd-64/
cp "$BUILD/bin_linux/bf2" "$BUILD/bin_linux/libdice_py.so" bin/amd-64/
chmod +x start.sh bin/amd-64/bf2
[ -e pb ] || ln -s pb_amd-64 pb

grep -E '^sv\.(serverPort|gameSpyPort|internet|punkBuster) ' $S
grep -E '^mm_rcon\.rcon(Port|Ip) |^mm_webadmin' $M || true
echo "RCON password: $RCON_PASSWORD"

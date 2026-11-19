#!/usr/bin/env bash
# Builds and runs the test container: a retail BF2 server image with OpenBF2 and the module in it.
#
# Usage (WSL or Git Bash):  testenv/run-docker.sh build|start|stop|status|logs|rm
#
# Environment (optional, except BASE for build):
#   BASE          build: the BF2 server image to build on (see Dockerfile)
#   BIN_DIR       dir with bf2 + libdice_py.so (~/bf2-linux-serverext-testenv-bullseye/build/bin_linux)
#   MODULE_DIR    dir with libserverext.so    (~/bf2-linux-serverext-testenv-bullseye/module)
#   BOT_PORT      16702   bot channel: baked into serverext.cfg as botListen 0.0.0.0:$BOT_PORT and
#                 published on 127.0.0.1 only (docs/configuration.md). Never 16567.
#   WATCHDOG_TICKS 6      baked into serverext.cfg
#   SERVEREXT_CFG_EXTRA   start: ';'-separated serverext.cfg lines appended in the container,
#                 e.g. "allowRoundRestart 1" or "allowRoundRestart 1;spawnFallbackSeconds 5"
#   TESTENV_NAME  image and container name   (serverext-testenv)
#   GAME_PORT     16571   published udp, same inside and out (the client's target port)
#   GAMESPY_PORT  29903   published udp
#   RCON_PORT     4723    published on 127.0.0.1, container side is ModManager's 4711
#   RCON_PASSWORD         random if unset; read it back with `status`
#   PUBLISH_IP    host address for the game/gamespy ports (default: 0.0.0.0, all interfaces). 127.0.0.1 keeps
#                 them off the LAN; a client on the same machine still joins.
#   EXTRA_ARGS    more `docker run` arguments
#   CONTAINER_CLI docker (default), or wslc for WSL containers (from Git Bash): see cli.sh
# Only containers/images named $TESTENV_NAME are touched.
set -euo pipefail
CMD=${1:?usage: run-docker.sh build|start|stop|status|logs|rm}
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=cli.sh
. "$HERE/cli.sh"
BIN_DIR=${BIN_DIR:-$HOME/bf2-linux-serverext-testenv-bullseye/build/bin_linux}
TESTENV_NAME=${TESTENV_NAME:-serverext-testenv}
GAME_PORT=${GAME_PORT:-16571}
GAMESPY_PORT=${GAMESPY_PORT:-29903}
RCON_PORT=${RCON_PORT:-4723}
MODULE_DIR=${MODULE_DIR:-$HOME/bf2-linux-serverext-testenv-bullseye/module}
BOT_PORT=${BOT_PORT:-16702}
WATCHDOG_TICKS=${WATCHDOG_TICKS:-6}
# Explicit, because wslc publishes on 127.0.0.1 unless told otherwise
PUBLISH_IP=${PUBLISH_IP:-0.0.0.0}:

for p in "$GAME_PORT" "$GAMESPY_PORT" "$RCON_PORT" "$BOT_PORT"; do
    case "$p" in 16567|16569|29900|29901|4711|4721) echo "refusing reserved port $p" >&2; exit 1;; esac
done

case "$CMD" in
build)
    ctx=$(mktemp -d)
    trap 'rm -rf "$ctx"' EXIT
    cp "$HERE/Dockerfile" "$BIN_DIR/bf2" "$BIN_DIR/libdice_py.so" "$MODULE_DIR/libserverext.so" "$ctx/"
    printf 'openbf2.addModDll serverext\n' > "$ctx/openbf2.con"
    # Inside the container the module must bind 0.0.0.0: the host cannot reach the container's
    # loopback. `start` publishes it on the host's 127.0.0.1 only.
    printf '# openbf2-server-ext bot channel (docs/protocol.md)\nbotListen 0.0.0.0:%s\nwatchdogTicks %s\n' \
        "$BOT_PORT" "$WATCHDOG_TICKS" > "$ctx/serverext.cfg"
    md5sum "$ctx/bf2" "$ctx/libdice_py.so" "$ctx/libserverext.so"
    "$CLI" build --build-arg BASE="${BASE:?set BASE to a BF2 server image}" -t "$TESTENV_NAME:latest" "$ctx"
    ;;
start)
    "$CLI" run -d --name "$TESTENV_NAME" \
        -e ENV_SERVER_NAME="serverext test" \
        -e ENV_MAX_PLAYERS=16 \
        -e ENV_SERVER_PORT="$GAME_PORT" \
        -e ENV_GAMESPY_PORT="$GAMESPY_PORT" \
        -e ENV_RCON_PASSWORD="${RCON_PASSWORD:-$(od -An -N9 -tx1 /dev/urandom | tr -d ' \n')}" \
        -e ENV_BF2WEBADMIN_HOST=127.0.0.1 \
        -e ENV_BF2WEBADMIN_PORT=4303 \
        -e SERVEREXT_CFG_EXTRA="${SERVEREXT_CFG_EXTRA:-}" \
        -p "$PUBLISH_IP$GAME_PORT:$GAME_PORT/udp" \
        -p "$PUBLISH_IP$GAMESPY_PORT:$GAMESPY_PORT/udp" \
        -p "127.0.0.1:$RCON_PORT:4711/tcp" \
        -p "127.0.0.1:$BOT_PORT:$BOT_PORT/tcp" \
        ${EXTRA_ARGS:-} \
        "$TESTENV_NAME:latest"
    ;;
stop) "$CLI" stop -t 15 "$TESTENV_NAME" ;;
rm)   "$CLI" rm -f "$TESTENV_NAME" ;;
status)
    "$CLI" ps -a --filter "name=^$TESTENV_NAME\$"
    procs "$TESTENV_NAME" | grep -E 'PID|bin/amd-64/bf2' || true
    # bound UDP sockets, matched by hex port in /proc/net/udp (the image has no ss/netstat)
    for p in "$GAME_PORT" "$GAMESPY_PORT"; do
        hex=$(printf '%04X' "$p")
        "$CLI" exec "$TESTENV_NAME" grep -q ":$hex " /proc/net/udp && echo "udp $p bound" || echo "udp $p NOT bound"
    done
    "$CLI" exec "$TESTENV_NAME" grep '^mm_rcon.rconPassword' /home/bf2/srv/mods/bf2/settings/modmanager.con || true
    "$CLI" exec "$TESTENV_NAME" sh -c 'cat /home/bf2/srv/mods/bf2/openbf2.con /home/bf2/srv/serverext.cfg' || true
    "$CLI" logs "$TESTENV_NAME" 2>&1 | grep -a '\[serverext\] loaded\|refusing to load\|could not load' || true
    ;;
logs)
    "$CLI" logs --tail 50 "$TESTENV_NAME" 2>&1
    "$CLI" exec "$TESTENV_NAME" sh -c 'tail -c 3000 /volume/bf2.log' | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' | tr -s '\n' || true
    ;;
*) echo "unknown command $CMD" >&2; exit 2 ;;
esac

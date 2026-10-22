#!/usr/bin/env bash
# Installs libserverext.so into a run directory made by prepare-content.sh.
#
# Usage (WSL):  testenv/deploy-module.sh <run-dir> [module-build-dir]
#   module-build-dir  default ~/serverext-build/linux-x64
#
# Environment (optional):
#   BOT_LISTEN      botListen for serverext.cfg (127.0.0.1:16700). Never port 16567.
#   WATCHDOG_TICKS  watchdogTicks (6)
#   ENTER_RANGE, SPAWN_FALLBACK_SECONDS, SPAWN_BLOCKED_SECONDS, ALLOW_ROUND_RESTART
#                   the v1 keys; written only when set (the module's defaults are 30, 5, 15 and 0)
#
# Writes mods/bf2/openbf2.con ("openbf2.addModDll serverext"), copies the module beside bf2 in
# bin/amd-64 (dlopen searches $ORIGIN, not the working directory) and writes serverext.cfg in the
# run directory, which is the server's working directory under start.sh.
set -euo pipefail
RUN=${1:?usage: deploy-module.sh <run-dir> [module-build-dir]}
BUILD=${2:-$HOME/serverext-build/linux-x64}
BOT_LISTEN=${BOT_LISTEN:-127.0.0.1:16700}
WATCHDOG_TICKS=${WATCHDOG_TICKS:-6}
case "$BOT_LISTEN" in *:16567) echo "refusing botListen port 16567" >&2; exit 1;; esac

cp "$BUILD/libserverext.so" "$RUN/bin/amd-64/libserverext.so"
printf 'openbf2.addModDll serverext\n' > "$RUN/mods/bf2/openbf2.con"
printf '# openbf2-server-ext bot channel (docs/protocol.md)\nbotListen %s\nwatchdogTicks %s\n' \
    "$BOT_LISTEN" "$WATCHDOG_TICKS" > "$RUN/serverext.cfg"
[ -n "${ENTER_RANGE:-}" ] && echo "enterRange $ENTER_RANGE" >> "$RUN/serverext.cfg"
[ -n "${SPAWN_FALLBACK_SECONDS:-}" ] && echo "spawnFallbackSeconds $SPAWN_FALLBACK_SECONDS" >> "$RUN/serverext.cfg"
[ -n "${SPAWN_BLOCKED_SECONDS:-}" ] && echo "spawnBlockedSeconds $SPAWN_BLOCKED_SECONDS" >> "$RUN/serverext.cfg"
[ -n "${ALLOW_ROUND_RESTART:-}" ] && echo "allowRoundRestart $ALLOW_ROUND_RESTART" >> "$RUN/serverext.cfg"
md5sum "$RUN/bin/amd-64/libserverext.so"
cat "$RUN/mods/bf2/openbf2.con" "$RUN/serverext.cfg"

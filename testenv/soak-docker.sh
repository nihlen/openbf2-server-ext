#!/usr/bin/env bash
# Samples the Docker test server once a minute: container state, bf2 uptime/CPU, bound UDP ports,
# the status screen's "Average FPS" redraw count (grows only while the main loop runs) and any
# [FRONTIER]/abort lines in bf2.log or the container's stderr.
#
# Usage:  testenv/soak-docker.sh [minutes]   (TESTENV_NAME, GAME_PORT, GAMESPY_PORT, CONTAINER_CLI as in run-docker.sh)
set -uo pipefail
# shellcheck source=cli.sh
. "$(dirname "$0")/cli.sh"
MIN=${1:-6}
N=${TESTENV_NAME:-serverext-testenv}
GP=$(printf '%04X' "${GAME_PORT:-16571}")
QP=$(printf '%04X' "${GAMESPY_PORT:-29903}")

for i in $(seq 0 "$MIN"); do
    [ "$i" -gt 0 ] && sleep 60
    is_running "$N" || { echo "$(date -u +%T) container not running"; break; }
    proc=$(procs "$N" | grep 'bin/amd-64/bf2' | awk '{print $2 "s", $3 "s", $4 "kB"}')
    [ -n "$proc" ] || { echo "$(date -u +%T) bf2 process GONE"; break; }
    udp=$("$CLI" exec "$N" sh -c "grep -c -E ':($GP|$QP) ' /proc/net/udp")
    fps=$("$CLI" exec "$N" sh -c "grep -a -o 'Average FPS:[0-9]*' /volume/bf2.log | wc -l; grep -a -o 'Average FPS:[0-9]*' /volume/bf2.log | tail -1" | xargs)
    echo "$(date -u +%T) etime/cpu/rss=$proc udp_sockets=$udp fps_redraws/last=$fps"
done
echo "frontier/abort lines: bf2.log=$("$CLI" exec "$N" grep -a -c -E '\[FRONTIER\]|Aborted|Segmentation' /volume/bf2.log) stderr=$("$CLI" logs "$N" 2>&1 | grep -a -c -E '\[FRONTIER\]|Aborted|Segmentation')"

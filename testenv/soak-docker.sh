#!/usr/bin/env bash
# Samples the Docker test server once a minute: container state, bf2 uptime/CPU, bound UDP ports,
# the status screen's "Average FPS" redraw count (grows only while the main loop runs) and any
# [FRONTIER]/abort lines in bf2.log or the container's stderr.
#
# Usage:  testenv/soak-docker.sh [minutes]   (TESTENV_NAME, GAME_PORT, GAMESPY_PORT as in run-docker.sh)
set -uo pipefail
MIN=${1:-6}
N=${TESTENV_NAME:-serverext-testenv}
GP=$(printf '%04X' "${GAME_PORT:-16571}")
QP=$(printf '%04X' "${GAMESPY_PORT:-29903}")

for i in $(seq 0 "$MIN"); do
    [ "$i" -gt 0 ] && sleep 60
    state=$(docker inspect -f '{{.State.Status}}' "$N" 2>/dev/null)
    [ "$state" = running ] || { echo "$(date -u +%T) container $state"; break; }
    proc=$(docker top "$N" -o pid,etime,time,rss,args | grep 'bin/amd-64/bf2' | awk '{print $2, $3, $4}')
    [ -n "$proc" ] || { echo "$(date -u +%T) bf2 process GONE"; break; }
    udp=$(docker exec "$N" sh -c "grep -c -E ':($GP|$QP) ' /proc/net/udp")
    fps=$(docker exec "$N" sh -c "grep -a -o 'Average FPS:[0-9]*' /volume/bf2.log | wc -l; grep -a -o 'Average FPS:[0-9]*' /volume/bf2.log | tail -1" | xargs)
    echo "$(date -u +%T) etime/cpu/rss=$proc udp_sockets=$udp fps_redraws/last=$fps"
done
echo "frontier/abort lines: bf2.log=$(docker exec "$N" grep -a -c -E '\[FRONTIER\]|Aborted|Segmentation' /volume/bf2.log) stderr=$(docker logs "$N" 2>&1 | grep -a -c -E '\[FRONTIER\]|Aborted|Segmentation')"

#!/usr/bin/env bash
# Samples a running WSL test server once a minute: process alive, uptime, CPU time, a GameSpy query
# round trip (map, game mode, state), the bound game port, the status screen's "Average FPS" redraw
# count (grows only while the main loop runs) and any [FRONTIER]/abort lines.
#
# Usage (WSL):  testenv/soak-check.sh [minutes] [run-dir] [gamespy-port]
set -uo pipefail
MIN=${1:-6}
RUN=${2:-$HOME/serverext-testenv/srv}
QPORT=${3:-29903}
PID=$(cat "$RUN/bf2.pid")
GP=$(sed -n 's/^sv.serverPort \([0-9]*\).*/\1/p' "$RUN/mods/bf2/settings/serversettings.con")

query() {
    python3 - "$QPORT" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.sendto(bytes([0xFE, 0xFD, 0, 1, 2, 3, 4, 0xFF, 0xFF, 0xFF, 1]), ("127.0.0.1", int(sys.argv[1])))
try:
    f = s.recv(8192).split(b"\0")
    kv = dict(zip(f[1::2], f[2::2]))
    print(" ".join(k + "=" + kv.get(k.encode(), b"?").decode(errors="replace")
                   for k in ("mapname", "gametype", "gamemode", "numplayers", "hostport")))
except Exception as e:
    print("query failed:", e)
EOF
}

for i in $(seq 0 "$MIN"); do
    [ "$i" -gt 0 ] && sleep 60
    if ! kill -0 "$PID" 2>/dev/null; then echo "$(date -u +%T) pid $PID GONE"; break; fi
    echo "$(date -u +%T) $(ps -o etime=,time=,rss= -p "$PID" | xargs) udp$GP=$(ss -Huln "sport = :$GP" | grep -c .) fps_redraws=$(grep -a -o 'Average FPS:[0-9]*' "$RUN/bf2.log" | wc -l) last=$(grep -a -o 'Average FPS:[0-9]*' "$RUN/bf2.log" | tail -1 | cut -d: -f2) $(query)"
done
echo "frontier/abort lines in bf2.log: $(grep -a -c -E '\[FRONTIER\]|Aborted|Segmentation' "$RUN/bf2.log")"

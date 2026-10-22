#!/usr/bin/env bash
# Starts / stops / inspects the OpenBF2 test server directly in WSL.
#
# Usage (WSL):  testenv/run-wsl.sh start|stop|status|log [run-dir]
#   run-dir defaults to ~/serverext-testenv/srv (made by prepare-content.sh)
#
# The server runs in the background through the image's own start.sh (LD_LIBRARY_PATH=bin/amd-64,
# no bf2hub LD_PRELOAD). Console output goes to <run-dir>/bf2.log, the PID to <run-dir>/bf2.pid.
set -euo pipefail
CMD=${1:?usage: run-wsl.sh start|stop|status|log [run-dir]}
RUN=${2:-$HOME/serverext-testenv/srv}
cd "$RUN"

pid() { [ -f bf2.pid ] && kill -0 "$(cat bf2.pid)" 2>/dev/null && cat bf2.pid; }
port() { sed -n 's/^sv.serverPort \([0-9]*\).*/\1/p' mods/bf2/settings/serversettings.con; }

case "$CMD" in
start)
    if p=$(pid); then echo "already running: pid $p"; exit 1; fi
    gp=$(port)
    case "$gp" in 16567|16569|"") echo "refusing sv.serverPort '$gp'" >&2; exit 1;; esac
    if ss -Huln "sport = :$gp" | grep -q .; then echo "udp $gp already bound" >&2; exit 1; fi
    # setsid + nohup + a short wait: when this is driven by a one-shot `wsl.exe -- ...`, returning
    # before the child has detached gets it killed with the session.
    TERM=xterm nohup setsid ./start.sh >bf2.log 2>&1 </dev/null &
    echo $! >bf2.pid
    sleep 3
    if ! kill -0 "$(cat bf2.pid)" 2>/dev/null; then echo "server exited at once; see $RUN/bf2.log" >&2; exit 1; fi
    echo "started pid $(cat bf2.pid), sv.serverPort $gp, log $RUN/bf2.log"
    ;;
stop)
    if p=$(pid); then
        kill -TERM "$p"; for _ in $(seq 20); do kill -0 "$p" 2>/dev/null || break; sleep 0.5; done
        if kill -0 "$p" 2>/dev/null; then kill -KILL "$p"; echo "pid $p ignored SIGTERM for 10 s; killed"
        else echo "stopped pid $p (SIGTERM)"; fi
    else
        echo "not running"
    fi
    rm -f bf2.pid
    ;;
status)
    if p=$(pid); then
        ps -o pid,etime,rss,pcpu,cmd -p "$p"
        ss -Hulnp | grep "pid=$p," || echo "(no UDP sockets)"
        ss -Htlnp | grep "pid=$p," || echo "(no TCP listeners)"
    else
        echo "not running"
    fi
    ;;
log) tail -n 60 bf2.log ;;
*) echo "unknown command $CMD" >&2; exit 2 ;;
esac

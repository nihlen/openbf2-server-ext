# shellcheck shell=bash
# Sourced by run-docker.sh and soak-docker.sh. The container CLI is docker, or wslc (WSL containers, from Git Bash) with
# CONTAINER_CLI=wslc. wslc has no `top`, no `inspect -f` and no `ps --format`, and the images have no ps, so what those
# gave is built here from commands both CLIs have.
CLI=${CONTAINER_CLI:-docker}

# is_running <container>
is_running() { [ -n "$("$CLI" ps -q --filter "name=^$1\$")" ]; }

# procs <container>: pid, elapsed and CPU seconds, RSS in kB and the command line of each process, read from /proc
procs() {
    "$CLI" exec "$1" sh -c '
        hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
        up=$(cut -d" " -f1 /proc/uptime)
        echo "PID ELAPSED_S CPU_S RSS_KB ARGS"
        for d in /proc/[0-9]*; do
            [ "${d#/proc/}" = "$$" ] && continue
            # [:cntrl:] for the NUL separators: a backslash would not survive a Windows command line
            args=$(tr -s "[:cntrl:]" " " < "$d/cmdline" 2>/dev/null) && [ -n "$args" ] || continue
            stat=$(cat "$d/stat" 2>/dev/null) || continue
            # the fields after "(comm) ": state is the first, utime the 12th, stime 13th, starttime 20th, rss 22nd
            # shellcheck disable=SC2086
            set -- ${stat##*) }
            echo "${d#/proc/} $(( ${up%.*} - ${20} / hz )) $(( (${12} + ${13}) / hz )) $(( ${22} * 4 )) $args"
        done'
}

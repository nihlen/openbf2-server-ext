# The production netsky server content with its retail bf2/libdice_py.so replaced by an OpenBF2
# linux-x64 build, plus the serverext module. Everything must target glibc <= 2.31 (Debian 11, the
# netsky base): use build-bullseye.sh, not the WSL Ubuntu build. Built and run by run-docker.sh.
#
# Build context (made by run-docker.sh build): bf2, libdice_py.so, libserverext.so, openbf2.con
# and serverext.cfg.
ARG BASE=cr.nihlen.net/library/bf2-netsky:2v2-nocheck
FROM ${BASE}

# run.sh moves /home/bf2/tmp/srv/* to /home/bf2/srv (the server's working directory) on the first
# start. The module goes beside bf2, because dlopen searches $ORIGIN and LD_LIBRARY_PATH, not the
# working directory; openbf2.con stays in mods/bf2 (only mods/bf2/settings moves to /volume).
COPY bf2 libdice_py.so libserverext.so /home/bf2/tmp/srv/bin/amd-64/
COPY openbf2.con /home/bf2/tmp/srv/mods/bf2/openbf2.con
COPY serverext.cfg /home/bf2/tmp/srv/serverext.cfg

# - start.sh instead of start_bf2hub.sh: no LD_PRELOAD of the retail libbf2hub.so into OpenBF2.
#   Server stderr (where [FRONTIER] diagnostics and the module's [serverext] lines go) stays on the
#   container's stderr: `docker logs`.
# - sv.internet 0 and sv.punkBuster 0: a private test server, never registered with a master.
# - SERVEREXT_CFG_EXTRA (optional, at `docker run`): ';'-separated lines appended to serverext.cfg
#   on the first start, e.g. "allowRoundRestart 1", so one image serves runs with different keys.
#   The baked file holds only botListen and watchdogTicks, so an appended key never repeats one.
# Everything else, including run.sh's {{placeholder}} substitution from ENV_*, is the image's own.
RUN set -e; \
    S=/home/bf2/tmp/srv/mods/bf2/settings/serversettings.con; \
    sed -i -e 's/^sv.internet .*/sv.internet 0/' -e 's/^sv.punkBuster .*/sv.punkBuster 0/' "$S"; \
    sed -i 's#\./start_bf2hub\.sh >bf2\.log#./start.sh >bf2.log#' run.sh; \
    grep -q '\./start\.sh >bf2\.log' run.sh; \
    sed -i 's#^    chmod +x ./start_bf2hub.sh ./bin/amd-64/bf2$#&\n    [ -z "$SERVEREXT_CFG_EXTRA" ] || echo "$SERVEREXT_CFG_EXTRA" | tr ";" "\\n" >> ./serverext.cfg#' run.sh; \
    grep -q 'SERVEREXT_CFG_EXTRA' run.sh; \
    chmod 755 /home/bf2/tmp/srv/bin/amd-64/bf2 /home/bf2/tmp/srv/bin/amd-64/libdice_py.so \
              /home/bf2/tmp/srv/bin/amd-64/libserverext.so; \
    chmod 644 /home/bf2/tmp/srv/mods/bf2/openbf2.con /home/bf2/tmp/srv/serverext.cfg

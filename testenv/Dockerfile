# The production netsky server content with its retail bf2/libdice_py.so replaced by an OpenBF2
# linux-x64 build. The build must target glibc <= 2.31 (Debian 11, the netsky base): use
# build-bullseye.sh, not the WSL Ubuntu build. Built and run by run-docker.sh.
#
# Build context: a directory holding bf2 and libdice_py.so (and later libserverext.so).
ARG BASE=cr.nihlen.net/library/bf2-netsky:2v2-nocheck
FROM ${BASE}

COPY bf2 libdice_py.so /home/bf2/tmp/srv/bin/amd-64/

# - start.sh instead of start_bf2hub.sh: no LD_PRELOAD of the retail libbf2hub.so into OpenBF2.
#   Server stderr (where [FRONTIER] diagnostics go) stays on the container's stderr: `docker logs`.
# - sv.internet 0 and sv.punkBuster 0: a private test server, never registered with a master.
# Everything else, including run.sh's {{placeholder}} substitution from ENV_*, is the image's own.
RUN set -e; \
    S=/home/bf2/tmp/srv/mods/bf2/settings/serversettings.con; \
    sed -i -e 's/^sv.internet .*/sv.internet 0/' -e 's/^sv.punkBuster .*/sv.punkBuster 0/' "$S"; \
    sed -i 's#\./start_bf2hub\.sh >bf2\.log#./start.sh >bf2.log#' run.sh; \
    grep -q '\./start\.sh >bf2\.log' run.sh; \
    chmod 755 /home/bf2/tmp/srv/bin/amd-64/bf2 /home/bf2/tmp/srv/bin/amd-64/libdice_py.so

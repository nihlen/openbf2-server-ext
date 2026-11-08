#!/usr/bin/env bash
# Builds the release zip for the production game-server images (bf2-docker images/netsky-openbf2-*): OpenBF2
# (bf2, libdice_py.so) with the IServerExtension commits, this module (libserverext.so) and OpenBF2-BF2Hub
# (libbf2hub.so), all in one zip that is unpacked into the server's bin/amd-64:
#
#   release/out/openbf2-serverext-<openbf2>-<module>-<bf2hub>-<backend>.zip   (7-character commits; backend: the
#                                                                         first label of GAMESPY_DOMAIN, e.g. bf2hub)
#   release/out/openbf2-serverext-<...>.zip.sha512                      (the line for bf2-docker's assets.sha512)
#   release/out/openbf2-serverext-<...>-debug.tar.gz                    (the same binaries with debug info, for gdb)
#
# The zip holds a source.txt with the three commits, the builder, and per binary its sha256, newest GLIBC_/GLIBCXX_
# symbol version and NEEDED libraries. It holds nothing about the machine or how the script was called, so the same
# commits give the same zip (checked: a clean and an incremental build gave identical binaries).
#
# Usage (Git Bash on Windows with Docker Desktop, or Linux with Docker):
#   release/build.sh
#
# Environment (optional):
#   OPENBF2_REPO, OPENBF2_REV   default ../OpenBF2 (C:\Data\Projects\OpenBF2), HEAD (branch serverext/r1)
#   MODULE_REPO, MODULE_REV     default this repo, HEAD
#   BF2HUB_REPO, BF2HUB_REV     default ../_ext/OpenBF2-BF2Hub (C:\Data\Projects\_ext\OpenBF2-BF2Hub), HEAD
#   OUT                         output directory (release/out)
#   JOBS                        parallel jobs (nproc)
#   CLEAN=1                     delete the build directories first (a full build, as in CI)
#   STRIP=0                     keep debug info in the zipped binaries
#   GAMESPY_DOMAIN              the GameSpy backend for the master list, CD-key checks and stats: bf2hub.p.bf2.org
#                               (BF2Hub, the default) or openspy.net (OpenBF2's source default: not in BF2Hub's list)
#
# Sources are commits, taken with `git archive` (uncommitted changes are reported and left out) and synced into the
# docker volume serverext-release-build by content (rsync -c), so an unchanged file keeps its timestamp and the
# OpenBF2 build stays incremental; the builder container is serverext-release-build and the image
# serverext-release-builder:jammy (release/Dockerfile). None of them are shared with the BF2WebAdmin localtest
# (bf2wa-openbf2-build). Run one build at a time.
set -euo pipefail
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

# Native Windows tools (git, docker) need C:/... paths: pwd -W in Git Bash (path conversion is off above)
abspath() { (cd "$1" && { pwd -W 2>/dev/null || pwd; }); }
HERE=$(abspath "$(dirname "$0")")
ROOT=$(abspath "$HERE/..")
OPENBF2_REPO=${OPENBF2_REPO:-$ROOT/../OpenBF2}
OPENBF2_REV=${OPENBF2_REV:-HEAD}
MODULE_REPO=${MODULE_REPO:-$ROOT}
MODULE_REV=${MODULE_REV:-HEAD}
BF2HUB_REPO=${BF2HUB_REPO:-$ROOT/../_ext/OpenBF2-BF2Hub}
BF2HUB_REV=${BF2HUB_REV:-HEAD}
OUT=${OUT:-$HERE/out}
JOBS=${JOBS:-$(nproc)}
CLEAN=${CLEAN:-0}
STRIP=${STRIP:-1}
GAMESPY_DOMAIN=${GAMESPY_DOMAIN:-bf2hub.p.bf2.org}
BUILDER=serverext-release-builder:jammy
VOLUME=serverext-release-build
CONTAINER=serverext-release-build

if [ -n "$(docker ps -q --filter "name=^$CONTAINER\$")" ]; then
    echo "a release build is already running (container $CONTAINER)" >&2
    exit 1
fi

# 1. The commits: resolved once, printed and recorded in source.txt.
sha() { git -C "$1" rev-parse --verify "$2^{commit}"; }
describe() { # repo rev -> the commit's subject (only what the commit determines, so the zip is reproducible)
    git -C "$1" log -1 --format=%s "$(sha "$1" "$2")" | cut -c1-100
}
OPENBF2_SHA=$(sha "$OPENBF2_REPO" "$OPENBF2_REV")
MODULE_SHA=$(sha "$MODULE_REPO" "$MODULE_REV")
BF2HUB_SHA=$(sha "$BF2HUB_REPO" "$BF2HUB_REV")
export OPENBF2_SOURCE MODULE_SOURCE BF2HUB_SOURCE
OPENBF2_SOURCE=$(describe "$OPENBF2_REPO" "$OPENBF2_REV")
MODULE_SOURCE=$(describe "$MODULE_REPO" "$MODULE_REV")
BF2HUB_SOURCE=$(describe "$BF2HUB_REPO" "$BF2HUB_REV")
NAME="openbf2-serverext-${OPENBF2_SHA:0:7}-${MODULE_SHA:0:7}-${BF2HUB_SHA:0:7}-${GAMESPY_DOMAIN%%.*}"
SOURCE_DATE_EPOCH=$(for r in "$OPENBF2_REPO:$OPENBF2_SHA" "$MODULE_REPO:$MODULE_SHA" "$BF2HUB_REPO:$BF2HUB_SHA"; do
    git -C "${r%:*}" log -1 --format=%ct "${r##*:}"; done | sort -n | tail -1)
echo "OpenBF2: $OPENBF2_SHA ($OPENBF2_REPO $OPENBF2_REV) $OPENBF2_SOURCE"
echo "module:  $MODULE_SHA ($MODULE_REPO $MODULE_REV) $MODULE_SOURCE"
echo "bf2hub:  $BF2HUB_SHA ($BF2HUB_REPO $BF2HUB_REV) $BF2HUB_SOURCE"
echo "zip:     $NAME.zip"
for r in "$OPENBF2_REPO" "$MODULE_REPO" "$BF2HUB_REPO"; do
    if [ -n "$(git -C "$r" status --porcelain --untracked-files=no)" ]; then
        echo "note: $r has uncommitted changes; they are NOT in this build (git archive of the commit)" >&2
    fi
done

# 2. The builder image.
docker build -q -t "$BUILDER" "$HERE" >/dev/null
BUILDER_IMAGE="release/Dockerfile from $(sed -n 's/^FROM //p' "$HERE/Dockerfile")"

# 3. Sources -> volume, synced by content.
docker volume create "$VOLUME" >/dev/null
if [ "$CLEAN" = 1 ]; then
    docker run --rm -v "$VOLUME:/w" "$BUILDER" rm -rf /w/build
fi
stream() { # name repo sha
    if [ "$(docker run --rm -v "$VOLUME:/w" "$BUILDER" cat "/w/src/$1.rev" 2>/dev/null)" = "$3" ]; then
        echo "$1: $3 already in $VOLUME"
        return
    fi
    echo "$1: syncing $3"
    git -C "$2" archive --format=tar "$3" | docker run -i --rm -v "$VOLUME:/w" "$BUILDER" sh -ec "
        rm -rf /w/new/$1 && mkdir -p /w/new/$1 /w/src/$1 && tar x -C /w/new/$1
        rsync -rlc --delete /w/new/$1/ /w/src/$1/
        rm -rf /w/new/$1 && echo $3 > /w/src/$1.rev"
}
stream openbf2 "$OPENBF2_REPO" "$OPENBF2_SHA"
stream module "$MODULE_REPO" "$MODULE_SHA"
stream bf2hub "$BF2HUB_REPO" "$BF2HUB_SHA"

# 4. Build, test and package inside the volume.
mkdir -p "$OUT"
docker run --rm --name "$CONTAINER" -v "$VOLUME:/w" \
    -e NAME="$NAME" -e OPENBF2_SHA="$OPENBF2_SHA" -e MODULE_SHA="$MODULE_SHA" -e BF2HUB_SHA="$BF2HUB_SHA" \
    -e OPENBF2_SOURCE -e MODULE_SOURCE -e BF2HUB_SOURCE -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -e BUILDER_IMAGE="$BUILDER_IMAGE" -e JOBS="$JOBS" -e STRIP="$STRIP" -e GAMESPY_DOMAIN="$GAMESPY_DOMAIN" \
    "$BUILDER" build-in-builder.sh 2>&1 | tee "$OUT/$NAME.build.log"

# 5. Outputs -> OUT
rm -f "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha512" "$OUT/$NAME-debug.tar.gz"
docker run --rm -v "$VOLUME:/w" "$BUILDER" tar c -C /w/out . | tar x -C "$OUT"
echo
echo "built $OUT/$NAME.zip"
cat "$OUT/$NAME.zip.sha512"

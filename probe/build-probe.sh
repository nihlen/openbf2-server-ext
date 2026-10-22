#!/usr/bin/env bash
# [TEMP] Builds libserverext_probe.so in WSL against an OpenBF2 checkout.
# Usage: probe/build-probe.sh <openbf2-checkout> [build-dir]
set -euo pipefail
SRC=${1:?usage: build-probe.sh <openbf2-checkout> [build-dir]}
OUT=${2:-$HOME/serverext-probe-build}
TOOLS=${TOOLS:-$HOME/tools}
export PATH="$TOOLS/cmake-3.30.5-linux-x86_64/bin:$TOOLS:$PATH"
HERE=$(cd "$(dirname "$0")" && pwd)
cmake -S "$HERE" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENBF2_SRC="$SRC"
cmake --build "$OUT"
ls -la "$OUT"/libserverext_probe.so

#!/usr/bin/env bash
# Builds libserverext.so and the unit tests in WSL (or Debian 11), then runs the tests.
# Usage: ./build-module.sh [openbf2-checkout] [build-dir]
#   openbf2-checkout  default: $OPENBF2_SRC, else /mnt/c/Data/Projects/OpenBF2 (the default checkout, branch main)
#   build-dir         default: ~/serverext-build/linux-x64 (the preset's)
# The checkout is always passed to CMake, so a build dir configured for another checkout (OPENBF2_SRC is a
# cache variable) follows the one given here. docs/system.md "Build" has the other builds.
set -euo pipefail
TOOLS=${TOOLS:-$HOME/tools}
export PATH="$TOOLS/cmake-3.30.5-linux-x86_64/bin:$TOOLS:$PATH"
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-${OPENBF2_SRC:-/mnt/c/Data/Projects/OpenBF2}}
args=(-DOPENBF2_SRC="$SRC")
[ -n "${2:-}" ] && args+=(-B "$2")
echo "OpenBF2: $SRC $(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo '(not a git checkout)')"
echo "module:  $HERE $(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || true)$(git -C "$HERE" diff --quiet HEAD 2>/dev/null || echo ' +uncommitted changes')"
cd "$HERE"
cmake --preset linux-x64 "${args[@]}"
OUT=${2:-$HOME/serverext-build/linux-x64}
cmake --build "$OUT"
(cd "$OUT" && ctest --output-on-failure)
ls -la "$OUT/libserverext.so"

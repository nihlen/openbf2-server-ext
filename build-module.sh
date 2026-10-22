#!/usr/bin/env bash
# Builds libserverext.so and the unit tests in WSL (or Debian 11), then runs the tests.
# Usage: ./build-module.sh [openbf2-checkout] [build-dir]
#   openbf2-checkout  default: the CMake default (the serverext-r1 worktree)
#   build-dir         default: ~/serverext-build/linux-x64 (the preset's)
set -euo pipefail
TOOLS=${TOOLS:-$HOME/tools}
export PATH="$TOOLS/cmake-3.30.5-linux-x86_64/bin:$TOOLS:$PATH"
HERE=$(cd "$(dirname "$0")" && pwd)
args=()
[ -n "${1:-}" ] && args+=(-DOPENBF2_SRC="$1")
[ -n "${2:-}" ] && args+=(-B "$2")
cd "$HERE"
cmake --preset linux-x64 "${args[@]}"
OUT=${2:-$HOME/serverext-build/linux-x64}
cmake --build "$OUT"
(cd "$OUT" && ctest --output-on-failure)
ls -la "$OUT/libserverext.so"

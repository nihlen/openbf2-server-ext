#!/usr/bin/env bash
# Builds the OpenBF2 linux-x64 dedicated server (bf2 + libdice_py.so) inside WSL.
#
# Usage (from WSL):  testenv/build-linux.sh <openbf2-checkout> [build-dir]
#   openbf2-checkout  OpenBF2 source tree, e.g. /mnt/c/Users/Alex/.t3/worktrees/OpenBF2/serverext-test-env
#   build-dir         agent-owned dir in the Linux filesystem (default ~/bf2-linux-serverext-testenv)
#
# Needs cmake >= 3.21, ninja, g++ and ncurses headers. Without root, run tools-linux.sh first: it
# puts cmake/ninja in ~/tools and unpacks libncurses-dev into ~/tools/sysroot. With root, simply
# `sudo apt install cmake ninja-build libncurses-dev` and the CURSES_* overrides below are harmless.
set -euo pipefail

SRC=${1:?usage: build-linux.sh <openbf2-checkout> [build-dir]}
OUT=${2:-$HOME/bf2-linux-serverext-testenv}
TOOLS=${TOOLS:-$HOME/tools}

export PATH="$TOOLS/cmake-3.30.5-linux-x86_64/bin:$TOOLS:$PATH"
LIBDIR=/usr/lib/x86_64-linux-gnu

curses_args=()
if [ ! -f /usr/include/curses.h ] && [ -f "$TOOLS/sysroot/usr/include/curses.h" ]; then
    # libncursesw.so (dev symlink) is missing without the -dev package; link the runtime directly.
    curses_args=(
        -DCURSES_INCLUDE_PATH="$TOOLS/sysroot/usr/include"
        -DCURSES_NCURSES_LIBRARY="$LIBDIR/libncursesw.so.6"
        -DCURSES_CURSES_LIBRARY="$LIBDIR/libncursesw.so.6"
        -DCURSES_FORM_LIBRARY="$LIBDIR/libformw.so.6"
        -DCURSES_EXTRA_LIBRARY="$LIBDIR/libtinfo.so.6"
    )
fi

cd "$SRC"
cmake --preset linux-x64 -B "$OUT" "${curses_args[@]}"
cmake --build "$OUT" --target bf2 dice_py_check_run
ls -la "$OUT/bin_linux"

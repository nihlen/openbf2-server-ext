#!/usr/bin/env bash
# Installs the OpenBF2 Linux build tools into ~/tools without root: cmake 3.30.5, ninja 1.12.1 and
# the libncurses-dev headers (unpacked from the .deb into ~/tools/sysroot). Idempotent.
set -euo pipefail
TOOLS=${TOOLS:-$HOME/tools}
mkdir -p "$TOOLS" && cd "$TOOLS"

if [ ! -x cmake-3.30.5-linux-x86_64/bin/cmake ]; then
    curl -sSLfo cmake.tgz https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz
    tar xzf cmake.tgz
fi
if [ ! -x ninja ]; then
    curl -sSLfo ninja.zip https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip
    python3 -c 'import zipfile; zipfile.ZipFile("ninja.zip").extractall(".")'
    chmod +x ninja
fi
if [ ! -f sysroot/usr/include/curses.h ] && [ ! -f /usr/include/curses.h ]; then
    apt-get download libncurses-dev
    mkdir -p sysroot && dpkg -x libncurses-dev_*.deb sysroot
fi
"$TOOLS/cmake-3.30.5-linux-x86_64/bin/cmake" --version | head -1
"$TOOLS/ninja" --version

#!/usr/bin/env bash
# Builds the OpenBF2 linux-x64 server inside debian:bullseye so the binary runs on a Debian 11
# image's glibc 2.31 (a WSL Ubuntu 22.04 build needs glibc 2.35 and fails to start there), and in
# the same container libserverext.so from this repo, with its unit tests.
#
# Usage (WSL):  testenv/build-bullseye.sh <openbf2-checkout> [build-dir]
#   build-dir defaults to ~/bf2-linux-serverext-testenv-bullseye (Linux filesystem)
#   Output: <build-dir>/build/bin_linux/{bf2,libdice_py.so} and <build-dir>/module/libserverext.so
#
# Environment (optional):
#   MODULE_SRC  this repo (default: the checkout holding this script); MODULE_SRC= skips the module
#   JOBS        parallel jobs (nproc)
# The sources are mounted read-only; only the build dir is written. Incremental re-runs reuse it.
set -euo pipefail
SRC=$(cd "${1:?usage: build-bullseye.sh <openbf2-checkout> [build-dir]}" && pwd)
OUT=${2:-$HOME/bf2-linux-serverext-testenv-bullseye}
MODULE_SRC=${MODULE_SRC-$(cd "$(dirname "$0")/.." && pwd)}
mkdir -p "$OUT"

module_mount=()
[ -n "$MODULE_SRC" ] && module_mount=(-v "$(cd "$MODULE_SRC" && pwd):/module:ro")

docker run --rm --name serverext-testenv-build \
    -v "$SRC:/src:ro" -v "$OUT:/out" "${module_mount[@]}" -w /src \
    -e JOBS="${JOBS:-$(nproc)}" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    debian:bullseye bash -euo pipefail -c '
        # Bullseye is past EOL: deb.debian.org 404s on its security pool, archive.debian.org serves it.
        printf "%s\n" \
            "deb http://archive.debian.org/debian bullseye main" \
            "deb http://archive.debian.org/debian bullseye-updates main" \
            "deb http://archive.debian.org/debian-security bullseye-security main" \
            >/etc/apt/sources.list
        apt-get update -qq
        apt-get install -y -qq g++ ninja-build libncurses-dev curl ca-certificates binutils >/dev/null
        if [ ! -x /out/.cmake/bin/cmake ]; then
            mkdir -p /out/.cmake
            curl -sSLf https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz \
                | tar xz --strip-components=1 -C /out/.cmake
        fi
        export PATH=/out/.cmake/bin:$PATH
        g++ --version | head -1; ldd --version | head -1
        cmake --preset linux-x64 -B /out/build
        cmake --build /out/build -j "$JOBS" --target bf2 dice_py_check_run
        if [ -d /module ]; then
            # The module preset writes to $HOME; point it at /out/module and at the mounted OpenBF2.
            cd /module
            cmake --preset linux-x64 -B /out/module -DOPENBF2_SRC=/src
            cmake --build /out/module -j "$JOBS"
            (cd /out/module && ctest --output-on-failure)
            # Debian 11 has no libpthread merge: list what the module needs and its newest GLIBC_ symbol.
            readelf -d /out/module/libserverext.so | grep NEEDED
            objdump -T /out/module/libserverext.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
        fi
        chown -R "$HOST_UID:$HOST_GID" /out
    '
ls -la "$OUT/build/bin_linux"
[ -z "$MODULE_SRC" ] || ls -la "$OUT/module/libserverext.so"

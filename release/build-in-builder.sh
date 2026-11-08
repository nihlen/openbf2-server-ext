#!/usr/bin/env bash
# Runs inside the release builder (release/Dockerfile), started by release/build.sh. Do not run it by hand.
#
# Expects the sources in /w/src/{openbf2,module,bf2hub} (git archive of the given commits) and builds into /w/build:
#   OpenBF2   linux-x64 preset, targets bf2 and dice_py_check_run (as OpenBF2's CI "Linux x64" job)
#   module    linux-x64 preset against /w/src/openbf2, then ctest (serverext_tests, serverext_admin_tests)
#   bf2hub    linux-x64 preset, target bf2hub
# Then writes /w/out/<NAME>.zip (bf2, libbf2hub.so, libdice_py.so, libserverext.so, source.txt: the contents of
# bin/amd-64), its .sha512, and <NAME>-debug.tar.gz with the unstripped binaries (for gdb; not deployed).
#
# Environment (set by build.sh): NAME, OPENBF2_SHA, MODULE_SHA, BF2HUB_SHA, OPENBF2_SOURCE, MODULE_SOURCE,
# BF2HUB_SOURCE, SOURCE_DATE_EPOCH, BUILDER_IMAGE, JOBS, STRIP (1: strip debug info from the zipped binaries),
# GAMESPY_DOMAIN (the GameSpy backend domain, see build.sh).
set -euo pipefail
: "${NAME:?}" "${OPENBF2_SHA:?}" "${MODULE_SHA:?}" "${BF2HUB_SHA:?}" "${SOURCE_DATE_EPOCH:?}" "${GAMESPY_DOMAIN:?}"
JOBS=${JOBS:-$(nproc)}
STRIP=${STRIP:-1}
W=/w
B=$W/build

for s in openbf2 module bf2hub; do
    [ -d "$W/src/$s" ] || { echo "missing source $W/src/$s" >&2; exit 1; }
done

echo "=== toolchain"
g++ --version | sed -n 1p
ldd --version | sed -n 1p
cmake --version | sed -n 1p
echo "libssl-dev $(dpkg-query -W -f='${Version}' libssl-dev)"

echo "=== OpenBF2 $OPENBF2_SHA"
# The GameSpy SDK's backend domain (gsPlatform.h GSI_DOMAIN_NAME: <game>.master, <game>.available, natneg; the source
# default is openspy.net). BF2Hub serves battlefield2.master.bf2hub.p.bf2.org, the backend the retail BF2Hub servers
# report to; the earlier openbf2-bf2hub-ccf3099-7e37854 zip was built this way. Without it the server is not in the
# BF2Hub server list and gets no CD-key checks.
GS_FLAGS="-DGSI_DOMAIN_NAME=\\\"${GAMESPY_DOMAIN}\\\""
(cd "$W/src/openbf2" && cmake --preset linux-x64 -B "$B/openbf2" -DBF2_GAME_DIR:PATH="$W/no-game-content" \
    "-DCMAKE_C_FLAGS=$GS_FLAGS" "-DCMAKE_CXX_FLAGS=$GS_FLAGS" >/dev/null)
cmake --build "$B/openbf2" -j "$JOBS" --target bf2 dice_py_check_run

echo "=== module $MODULE_SHA"
(cd "$W/src/module" && cmake --preset linux-x64 -B "$B/module" -DOPENBF2_SRC="$W/src/openbf2" >/dev/null)
cmake --build "$B/module" -j "$JOBS"
echo "=== module ctest"
(cd "$B/module" && ctest --output-on-failure)

echo "=== bf2hub $BF2HUB_SHA"
# bf2hub hands the retail GameSpy stats/BF2Web names to Python (it expects an external redirect); point them at the
# same backend domain, as the earlier openbf2-bf2hub zip did. A patched copy keeps the synced source equal to the commit.
rm -rf "$W/src/bf2hub-patched" && cp -a "$W/src/bf2hub" "$W/src/bf2hub-patched"
sed -i "s/\.gamespy\.com\"/.${GAMESPY_DOMAIN}\"/g" "$W/src/bf2hub-patched/src/PythonModule.cpp" "$W/src/bf2hub-patched/src/Snapshot.cpp"
(cd "$W/src/bf2hub-patched" && cmake --preset linux-x64 -B "$B/bf2hub-patched" >/dev/null)
cmake --build "$B/bf2hub-patched" -j "$JOBS" --target bf2hub

echo "=== package $NAME"
FILES="bf2 libbf2hub.so libdice_py.so libserverext.so"
STAGE=$W/stage/$NAME
DEBUG=$W/stage/$NAME-debug
rm -rf "$W/stage" "$W/out"
mkdir -p "$STAGE" "$DEBUG" "$W/out"
cp "$B/openbf2/bin_linux/bf2" "$B/openbf2/bin_linux/libdice_py.so" "$B/module/libserverext.so" "$B/bf2hub-patched/libbf2hub.so" "$DEBUG/"
for f in $FILES; do
    if [ "$STRIP" = 1 ]; then
        strip --strip-debug -o "$STAGE/$f" "$DEBUG/$f"
    else
        cp "$DEBUG/$f" "$STAGE/$f"
    fi
done

# Every binary must load on Debian trixie (glibc 2.41) and must not need a libstdc++ newer than the image's.
max_version() { objdump -T "$1" | { grep -oE "$2_[0-9.]+" || true; } | sort -uV | tail -1; }
glibc_ok=1
{
    echo "name $NAME"
    echo "openbf2 $OPENBF2_SHA $OPENBF2_SOURCE"
    echo "module $MODULE_SHA $MODULE_SOURCE"
    echo "bf2hub $BF2HUB_SHA $BF2HUB_SOURCE"
    echo "builder $BUILDER_IMAGE: $(g++ --version | sed -n 1p), $(ldd --version | sed -n 1p), libssl-dev $(dpkg-query -W -f='${Version}' libssl-dev)"
    echo "gamespy-domain $GAMESPY_DOMAIN (OpenBF2 GSI_DOMAIN_NAME; bf2hub's stats and BF2Web names)"
    echo "flags OpenBF2 preset linux-x64 (RelWithDebInfo, static libstdc++/libgcc), module preset linux-x64, bf2hub preset linux-x64; debug info $([ "$STRIP" = 1 ] && echo stripped || echo kept)"
    for f in $FILES; do
        g=$(max_version "$STAGE/$f" GLIBC)
        x=$(max_version "$STAGE/$f" GLIBCXX)
        n=$(readelf -d "$STAGE/$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
        echo "file $f sha256 $(sha256sum "$STAGE/$f" | cut -d' ' -f1) max ${g:-none} ${x:-no-GLIBCXX} needs ${n% }"
        [ -z "$g" ] || [ "$(printf '%s\n' "$g" GLIBC_2.41 | sort -V | tail -1)" = GLIBC_2.41 ] || glibc_ok=0
    done
} > "$STAGE/source.txt"
cat "$STAGE/source.txt"
[ "$glibc_ok" = 1 ] || { echo "a binary needs a glibc newer than 2.41 (Debian trixie)" >&2; exit 1; }
grep -q 'libserverext.so .*libssl.so.3' "$STAGE/source.txt" || { echo "libserverext.so does not link OpenSSL 3" >&2; exit 1; }
grep -aqF "%s.master.$GAMESPY_DOMAIN" "$STAGE/bf2" || { echo "bf2 does not report to %s.master.$GAMESPY_DOMAIN" >&2; exit 1; }
if [ "$GAMESPY_DOMAIN" != openspy.net ] && grep -aqF 'openspy.net' "$STAGE/bf2"; then echo "bf2 still names openspy.net" >&2; exit 1; fi
if grep -aqF '.gamespy.com' "$STAGE/libbf2hub.so"; then echo "libbf2hub.so still names gamespy.com" >&2; exit 1; fi

# Reproducible archive: fixed order, fixed timestamps (the newest source commit), no extra attributes.
chmod 755 "$STAGE"/bf2 "$STAGE"/*.so
chmod 644 "$STAGE/source.txt"
touch -d "@$SOURCE_DATE_EPOCH" "$STAGE"/*
(cd "$STAGE" && zip -X -9 -q "$W/out/$NAME.zip" $FILES source.txt)
(cd "$W/out" && sha512sum "$NAME.zip" > "$NAME.zip.sha512")
cp "$STAGE/source.txt" "$DEBUG/source.txt"
touch -d "@$SOURCE_DATE_EPOCH" "$DEBUG"/*
tar -C "$W/stage" --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$SOURCE_DATE_EPOCH" -czf "$W/out/$NAME-debug.tar.gz" "$NAME-debug"
unzip -l "$W/out/$NAME.zip"
cat "$W/out/$NAME.zip.sha512"

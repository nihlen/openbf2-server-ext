#!/usr/bin/env bash
# Runs inside the CentOS 7 builder (release/centos7/Dockerfile), started by release/centos7/build.sh. Do not run it
# by hand. It is release/build-in-builder.sh for a glibc 2.17 target; what differs:
#   OpenBF2   every source file gets glibc217-compat.h (see that file)
#   module    OpenSSL from /opt/openssl is linked statically and its symbols are kept out of the module's exports
#   bf2hub    libstdc++ and libgcc are linked statically: CentOS 7's libstdc++ is GCC 4.8's
#   all       DWARF 4 debug info, which CentOS 7's gdb and the builder's binutils read
#   checks    no binary may need a glibc newer than 2.17, a libstdc++ or an OpenSSL from the system
#
# Expects the sources in /w/src/{openbf2,module,bf2hub} (git archive of the given commits) and builds into /w/build.
# Writes /w/out/<NAME>.zip (bf2, libbf2hub.so, libdice_py.so, libserverext.so, source.txt: the contents of
# bin/amd-64), its .sha512, and <NAME>-debug.tar.gz with the unstripped binaries (for gdb; not deployed).
#
# Environment (set by build.sh): NAME, OPENBF2_SHA, MODULE_SHA, BF2HUB_SHA, OPENBF2_SOURCE, MODULE_SOURCE,
# BF2HUB_SOURCE, SOURCE_DATE_EPOCH, BUILDER_IMAGE, JOBS, STRIP (1: strip debug info from the zipped binaries),
# GAMESPY_DOMAIN (the GameSpy backend domain, see release/build.sh).
set -euo pipefail
: "${NAME:?}" "${OPENBF2_SHA:?}" "${MODULE_SHA:?}" "${BF2HUB_SHA:?}" "${SOURCE_DATE_EPOCH:?}" "${GAMESPY_DOMAIN:?}"
JOBS=${JOBS:-$(nproc)}
STRIP=${STRIP:-1}
W=/w
B=$W/build
COMPAT=/usr/local/share/glibc217-compat.h
GLIBC_MAX=GLIBC_2.17
DEBUG_FLAGS=-gdwarf-4
OPENSSL_VERSION=$(sed -n 's/^Version: //p' /opt/openssl/lib/pkgconfig/openssl.pc)

for s in openbf2 module bf2hub; do
    [ -d "$W/src/$s" ] || { echo "missing source $W/src/$s" >&2; exit 1; }
done

echo "=== toolchain"
g++ --version | sed -n 1p
ldd --version | sed -n 1p
cmake --version | sed -n 1p
echo "OpenSSL $OPENSSL_VERSION (static, /opt/openssl)"

echo "=== OpenBF2 $OPENBF2_SHA"
# GSI_DOMAIN_NAME: the GameSpy backend, as in release/build-in-builder.sh.
ENGINE_FLAGS="-DGSI_DOMAIN_NAME=\\\"${GAMESPY_DOMAIN}\\\" -include $COMPAT $DEBUG_FLAGS"
(cd "$W/src/openbf2" && cmake --preset linux-x64 -B "$B/openbf2" -DBF2_GAME_DIR:PATH="$W/no-game-content" \
    "-DCMAKE_C_FLAGS=$ENGINE_FLAGS" "-DCMAKE_CXX_FLAGS=$ENGINE_FLAGS" >/dev/null)
cmake --build "$B/openbf2" -j "$JOBS" --target bf2 dice_py_check_run

echo "=== module $MODULE_SHA"
(cd "$W/src/module" && cmake --preset linux-x64 -B "$B/module" -DOPENBF2_SRC="$W/src/openbf2" \
    -DOPENSSL_ROOT_DIR=/opt/openssl -DOPENSSL_USE_STATIC_LIBS=TRUE \
    "-DCMAKE_C_FLAGS=$DEBUG_FLAGS" "-DCMAKE_CXX_FLAGS=$DEBUG_FLAGS" \
    "-DCMAKE_MODULE_LINKER_FLAGS=-Wl,--exclude-libs,libssl.a:libcrypto.a" >/dev/null)
cmake --build "$B/module" -j "$JOBS"
echo "=== module ctest"
(cd "$B/module" && ctest --output-on-failure)

echo "=== bf2hub $BF2HUB_SHA"
# The stats and BF2Web names, as in release/build-in-builder.sh: a patched copy keeps the synced source equal to the
# commit.
rm -rf "$W/src/bf2hub-patched" && cp -a "$W/src/bf2hub" "$W/src/bf2hub-patched"
sed -i "s/\.gamespy\.com\"/.${GAMESPY_DOMAIN}\"/g" "$W/src/bf2hub-patched/src/PythonModule.cpp" "$W/src/bf2hub-patched/src/Snapshot.cpp"
STATIC_RUNTIME="-static-libstdc++ -static-libgcc"
(cd "$W/src/bf2hub-patched" && cmake --preset linux-x64 -B "$B/bf2hub-patched" "-DCMAKE_CXX_FLAGS=$DEBUG_FLAGS" \
    "-DCMAKE_SHARED_LINKER_FLAGS=$STATIC_RUNTIME" "-DCMAKE_MODULE_LINKER_FLAGS=$STATIC_RUNTIME" >/dev/null)
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

max_version() { objdump -T "$1" | { grep -oE "$2_[0-9.]+" || true; } | sort -uV | tail -1; }
ok=1
{
    echo "name $NAME"
    echo "openbf2 $OPENBF2_SHA $OPENBF2_SOURCE"
    echo "module $MODULE_SHA $MODULE_SOURCE"
    echo "bf2hub $BF2HUB_SHA $BF2HUB_SOURCE"
    echo "builder $BUILDER_IMAGE: $(g++ --version | sed -n 1p) built in the image, $(ldd --version | sed -n 1p)"
    echo "openssl $OPENSSL_VERSION linked statically into libserverext.so"
    echo "gamespy-domain $GAMESPY_DOMAIN (OpenBF2 GSI_DOMAIN_NAME; bf2hub's stats and BF2Web names)"
    echo "flags OpenBF2 preset linux-x64 (RelWithDebInfo, static libstdc++/libgcc) with glibc217-compat.h, module preset linux-x64, bf2hub preset linux-x64 with static libstdc++/libgcc; debug info $([ "$STRIP" = 1 ] && echo stripped || echo kept)"
    for f in $FILES; do
        g=$(max_version "$STAGE/$f" GLIBC)
        x=$(max_version "$STAGE/$f" GLIBCXX)
        n=$(readelf -d "$STAGE/$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
        echo "file $f sha256 $(sha256sum "$STAGE/$f" | cut -d' ' -f1) max ${g:-none} ${x:-no-GLIBCXX} needs ${n% }"
        [ -z "$g" ] || [ "$(printf '%s\n' "$g" "$GLIBC_MAX" | sort -V | tail -1)" = "$GLIBC_MAX" ] || ok=0
        [ -z "$x" ] || ok=0
    done
} > "$STAGE/source.txt"
cat "$STAGE/source.txt"
[ "$ok" = 1 ] || { echo "a binary needs a glibc newer than ${GLIBC_MAX#GLIBC_} or the system's libstdc++" >&2; exit 1; }
if grep -E '^file .* needs .*lib(ssl|crypto|stdc\+\+|gcc_s)\.so' "$STAGE/source.txt"; then
    echo "a binary needs OpenSSL, libstdc++ or libgcc_s from the system" >&2; exit 1
fi
if objdump -T "$STAGE/libserverext.so" | grep -E ' (SSL|EVP|CRYPTO|OPENSSL|BIO|X509|ERR)_[A-Za-z0-9_]+$'; then
    echo "libserverext.so exports OpenSSL symbols" >&2; exit 1
fi
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
# CentOS 7's tar has no --sort: the sorted list is passed in.
(cd "$W/stage" && find "$NAME-debug" -print0 | LC_ALL=C sort -z \
    | tar --no-recursion --null -T - --owner=0 --group=0 --numeric-owner --mtime="@$SOURCE_DATE_EPOCH" -cf - \
    | gzip -n > "$W/out/$NAME-debug.tar.gz")
unzip -l "$W/out/$NAME.zip"
cat "$W/out/$NAME.zip.sha512"

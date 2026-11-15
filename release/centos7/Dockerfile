# The CentOS 7 builder: the same four binaries as release/Dockerfile, for servers that still run CentOS 7
# (glibc 2.17, OpenSSL 1.0.2, ncurses 5). Used by release/centos7/build.sh (locally and in
# .github/workflows/release-centos7.yml).
#
# manylinux2014 is CentOS 7.9 with a current CMake. On top of it:
#   - GCC 11.4.0, built here. The image's own GCC 10 (devtoolset-10) only has the old std::string ABI, and OpenBF2's
#     Linux port asserts the new one (Code/BF2/IO/File/FileManager.h). 11.4 is also the compiler of the regular
#     release (Ubuntu 22.04), so both releases come from the same compiler version. Not bootstrapped: the libraries
#     that end up in the binaries (libstdc++, libgcc) are built by the new compiler either way.
#   - OpenSSL 3.5 (LTS), as static libraries in /opt/openssl. CentOS 7's OpenSSL 1.0.2 has no TLS 1.3, which the admin
#     channel requires, so libserverext.so carries its own. It never reads an openssl.cnf (no-autoload-config) and
#     loads no provider modules (no-module).
# The base is pinned by digest and the two sources by checksum; the yum packages are what the CentOS 7 vault has.
FROM quay.io/pypa/manylinux2014_x86_64@sha256:e6a785657009c612fe2a85fa933051d03a50ba62b1c5e47d62abcab546a2e423

RUN yum install -y -q ncurses-devel perl-core rsync zip unzip file bzip2 \
 && yum clean all \
 && pipx install ninja==1.13.2

ARG GCC_VERSION=11.4.0
ARG GCC_SHA512=a5018bf1f1fa25ddf33f46e720675d261987763db48e7a5fdf4c26d3150a8abcb82fdc413402df1c32f2e6b057d9bae6bdfa026defc4030e10144a8532e60f14
RUN cd /tmp \
 && curl -sSLfO "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz" \
 && echo "${GCC_SHA512}  gcc-${GCC_VERSION}.tar.xz" | sha512sum -c - \
 && tar xf "gcc-${GCC_VERSION}.tar.xz" \
 && cd "gcc-${GCC_VERSION}" && ./contrib/download_prerequisites \
 && mkdir /tmp/gcc-build && cd /tmp/gcc-build \
 && "../gcc-${GCC_VERSION}/configure" --prefix=/opt/gcc11 --enable-languages=c,c++ --disable-multilib \
        --disable-bootstrap --disable-nls --enable-checking=release --with-default-libstdcxx-abi=new \
        --disable-libsanitizer --disable-libgomp --disable-libitm --disable-libvtv --disable-libquadmath \
        --disable-libssp >/dev/null \
 && { make -j"$(nproc)" >/dev/null 2>/tmp/gcc-make.err || { tail -50 /tmp/gcc-make.err; exit 1; }; } \
 && make install-strip >/dev/null \
 && cd /tmp && rm -rf gcc-*

# LD_LIBRARY_PATH: the module's unit tests link libstdc++ dynamically and CentOS 7's own is GCC 4.8's.
ENV PATH=/opt/gcc11/bin:$PATH CC=/opt/gcc11/bin/gcc CXX=/opt/gcc11/bin/g++ LD_LIBRARY_PATH=/opt/gcc11/lib64

ARG OPENSSL_VERSION=3.5.9
ARG OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
# SOURCE_DATE_EPOCH (2026-01-01): OpenSSL compiles its build time into the library ("built on"), which would make
# libserverext.so differ with every build of this image.
RUN cd /tmp \
 && export SOURCE_DATE_EPOCH=1767225600 \
 && curl -sSLfO "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" \
 && echo "${OPENSSL_SHA256}  openssl-${OPENSSL_VERSION}.tar.gz" | sha256sum -c - \
 && tar xzf "openssl-${OPENSSL_VERSION}.tar.gz" \
 && cd "openssl-${OPENSSL_VERSION}" \
 && ./Configure linux-x86_64 no-shared no-module no-autoload-config no-tests no-docs no-apps -fPIC \
        --prefix=/opt/openssl --libdir=lib >/dev/null \
 && make -j"$(nproc)" >/dev/null \
 && make install_sw >/dev/null \
 && cd /tmp && rm -rf openssl-*

COPY glibc217-compat.h /usr/local/share/glibc217-compat.h
COPY build-in-builder.sh /usr/local/bin/build-in-builder.sh
RUN chmod 755 /usr/local/bin/build-in-builder.sh

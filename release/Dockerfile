# The release builder: OpenBF2 (bf2, libdice_py.so), libserverext.so and libbf2hub.so for the production game-server
# images. Used by release/build.sh (locally and in .github/workflows/release.yml).
#
# Ubuntu 22.04 because:
#   - it is OpenBF2's own Linux CI target (.github/workflows/build.yml "Linux x64", docs/ci-releases.md: "Linux
#     binaries target the Ubuntu 22.04 system-library baseline"), so the engine is built with the compiler its CI
#     proves (GCC 11) and the preset's static libstdc++/libgcc;
#   - glibc 2.35 is older than Debian trixie's 2.41, so the binaries run on the trixie images (and on Debian 12 or
#     Ubuntu 22.04 and newer);
#   - it ships OpenSSL 3.0.2, the version the module's admin channel was tested against; the module links libssl.so.3
#     dynamically and trixie's libssl3t64 (3.5) is ABI-compatible.
# The digest pins the base; apt packages are whatever the 22.04 archive has at build time (build.sh records the
# versions in source.txt).
FROM ubuntu:22.04@sha256:076e7bf82319619f28dd0aa9c02ab34b055ad37100395b4f49a2ffba04fb5e11

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        cmake ninja-build g++ libncurses-dev libssl-dev python3 binutils file rsync zip unzip ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY build-in-builder.sh /usr/local/bin/build-in-builder.sh
RUN chmod 755 /usr/local/bin/build-in-builder.sh

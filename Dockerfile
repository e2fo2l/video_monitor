# Build environment for video_monitor (see README.md, "Building"). The image holds the robot
# cross toolchain, xmake and the Debian headers. The robot's own libraries are proprietary, so
# they are mounted at build time instead of being baked in:
#
#     docker build -t video_monitor-build .
#     docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src -v "$PWD/sysroot/usr/lib":/robot-lib:ro \
#                -v video_monitor-xmake:/opt/xmake-global video_monitor-build          # robot
#     docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src \
#                -v video_monitor-xmake:/opt/xmake-global video_monitor-build pc       # PC
#
# The video_monitor-xmake volume caches xmake's packages; the PC build compiles OpenSSL, curl,
# ALSA and FFmpeg from source on its first run.

FROM debian:trixie-slim

# Native compilers and build tools are for the PC build's packages and for CMake-based packages.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates curl git unzip bzip2 xz-utils binutils libncurses6 \
        cmake ninja-build make gcc g++ perl pkg-config nasm yasm \
        autoconf automake libtool m4 python3 \
 && rm -rf /var/lib/apt/lists/*

# 1. Toolchain. Its glibc must not be newer than the robot's (2021.11-1: glibc 2.34).
ARG TOOLCHAIN_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/aarch64--glibc--stable-2021.11-1.tar.bz2
RUN mkdir /opt/toolchain \
 && curl -sSfL "$TOOLCHAIN_URL" | tar xj -C /opt/toolchain --strip-components=1 \
 && /opt/toolchain/bin/aarch64-linux-gcc --version | head -1

# xmake, as a single self-contained binary.
ARG XMAKE_VERSION=3.1.1
RUN curl -sSfL -o /usr/local/bin/xmake \
        "https://github.com/xmake-io/xmake/releases/download/v${XMAKE_VERSION}/xmake-bundle-v${XMAKE_VERSION}.linux.x86_64" \
 && chmod +x /usr/local/bin/xmake

# 3. Headers matching the robot's libraries, from Debian bullseye's arm64 -dev packages.
ARG DEBIAN_PACKAGES="libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libssl-dev libcurl4-openssl-dev libasound2-dev"
RUN set -e; mkdir -p /opt/robot-headers/usr/include /tmp/debs; cd /tmp/debs; \
    for MIRROR in https://deb.debian.org/debian https://archive.debian.org/debian; do \
        curl -sSfL "$MIRROR/dists/bullseye/main/binary-arm64/Packages.xz" | xz -d > Packages && break; \
    done; \
    for p in $DEBIAN_PACKAGES; do \
        f=$(awk -v p="$p" '$1=="Package:"{c=$2} c==p && $1=="Filename:"{print $2; exit}' Packages); \
        curl -sSfLO "$MIRROR/$f"; \
        rm -rf x && mkdir x && (cd x && ar x "../${f##*/}" && tar xf data.tar.* ./usr/include \
            && cp -a usr/include/. /opt/robot-headers/usr/include/); \
    done; \
    cd / && rm -rf /tmp/debs

# xmake state lives outside the mounted source tree, so the container never touches the host's
# .xmake/ configuration. The global dir keeps the package repository and rapidjson; it is
# world-writable so the container also works when run with --user.
ENV XMAKE_ROOT=y \
    XMAKE_GLOBALDIR=/opt/xmake-global \
    XMAKE_CONFIGDIR=/tmp/xmake-config
RUN mkdir -p /opt/xmake-global && xmake repo --update && chmod -R a+rwX /opt/xmake-global

COPY tools/docker-build.sh /usr/local/bin/docker-build.sh
WORKDIR /src
ENTRYPOINT ["/usr/local/bin/docker-build.sh"]

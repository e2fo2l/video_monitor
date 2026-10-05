#!/bin/sh
# Container entrypoint (see Dockerfile). Builds the source tree mounted on /src.
#
#     docker-build.sh [robot|pc] [xmake f options...]
#
# robot (default): cross build against the robot libraries mounted on /robot-lib.
# pc: x86_64 debug build with the vendor SDK stubs, for development and tools/host_test.sh.
# Extra arguments go to `xmake f`, e.g. --compat=y or --breakpad=y.
set -e

TARGET=robot
case "$1" in
    robot|pc) TARGET=$1; shift ;;
esac

if [ "$TARGET" = pc ]; then
    xmake f -y -p linux -a x86_64 -m debug -o build "$@"
    xmake -y
    echo "built build/linux/x86_64/debug/video_monitor"
    exit 0
fi

if [ ! -e /robot-lib/libagora-rtc-sdk.so ]; then
    echo "error: mount the robot's /usr/lib copy on /robot-lib (README step 2)" >&2
    exit 1
fi

# vendor_root as xmake.lua expects it: the robot's libraries next to the Debian headers.
VENDOR_ROOT=/tmp/vendor_root
mkdir -p "$VENDOR_ROOT/usr"
ln -sfn /robot-lib "$VENDOR_ROOT/usr/lib"
ln -sfn /opt/robot-headers/usr/include "$VENDOR_ROOT/usr/include"

xmake f -y -p linux -a arm64 -m release --sdk=/opt/toolchain --cross=aarch64-linux- \
        --vendor_root="$VENDOR_ROOT" -o build-robot "$@"
xmake -y

BIN=build-robot/linux/arm64/release/video_monitor
echo "built $BIN (needs glibc $(readelf -V "$BIN" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1 | cut -d_ -f2))"

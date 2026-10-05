# video_monitor: reverse-engineered into C++20

> **AI disclosure:** this project was reverse engineered and written with the help of an AI
> assistant (Claude), which was used to analyze the decompiled binary, reconstruct the code and
> write the documentation.

A reverse engineering of `video_monitor`, the camera service older Dreame vacuums shipped
(Allwinner sunxi, aarch64; original version string `commit-r2212_0041release-0-g8da91c04`),
rewritten as C++20 source from the decompiled original binary. The service takes H.264
frames from the Allwinner `libtrecorder` encoder and pushes them into an Agora RTC channel for
the app's live view. It also handles intercom, recording, playback and cloud upload, and it
talks to the `ava` daemon over nanomsg. Newer firmware no longer ships it, but it still has
every library it needs.

Each function carries a `// 0x...` comment with its address in the original binary, and
places where it deviates from the original are marked `NOTE(orig)`. By default it builds with
fixes for the original's bugs; `--compat=y` builds the original behavior instead (see
[Build options](#build-options)).

## Building for the robot

The robot has no compiler, so you cross-compile on an x86_64 Linux machine. The easiest way is
the container image in this repository, which brings the cross toolchain, xmake and the headers.
You need:

- [Docker](https://docs.docker.com/engine/install/)
- SSH access to your robot

The build links against the robot's own libraries (Agora SDK, Allwinner media stack, nnxx,
FFmpeg 4, OpenSSL 1.1, ...). They are proprietary and are not part of this repository, so step 1
copies them from your robot. Nothing on the robot is modified.

Run every command from the repository root. The copy of the robot's libraries ends up in the
repository folder, and `.gitignore` keeps it out of git. Set your robot's address first:

```sh
ROBOT=root@192.168.1.50          # your robot
```

### 1. Copy the robot's libraries

```sh
mkdir -p sysroot/usr
ssh $ROBOT 'tar c -C /usr lib' | tar x -C sysroot/usr
ln -sfn usr/lib sysroot/lib
```

This assumes `/lib` is a symlink to `/usr/lib` on the robot (check with `ssh $ROBOT ls -ld /lib`).
If it isn't, copy `/lib` as well. Don't redistribute this directory.

### 2. Build

```sh
docker build -t video_monitor-build .
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src -v "$PWD/sysroot/usr/lib":/robot-lib:ro \
           -v video_monitor-xmake:/opt/xmake-global video_monitor-build
```

The binary ends up in `build-robot/linux/arm64/release/video_monitor`.

- **Options:** arguments after the image name go to `xmake f`, e.g. `video_monitor-build --compat=y`
  (see [Build options](#build-options)).
- **Cache:** the `video_monitor-xmake` volume keeps the packages xmake downloads between builds.
- **glibc:** the image's toolchain targets glibc 2.34 (tested on an r2416). The binary must not
  need a newer glibc than the robot has; check the robot's with
  `ssh $ROBOT /lib/libc.so.6 | head -1`. For an older robot, rebuild the image with an older
  toolchain from <https://toolchains.bootlin.com/releases_aarch64.html>:
  `docker build --build-arg TOOLCHAIN_URL=<tarball URL> -t video_monitor-build .`

### Build options

By default the build includes these fixes:

- **Single-instance check.** The original counts how many times the text `video_monitor`
  appears in `ps aux` output, so with a `ps` that prints full command lines it sees itself and
  exits with `video_monitor is Running, Please Check`. The check also reads into a fixed
  256-byte buffer, so a long `ps` line crashes it. The fixed build looks for other
  `video_monitor` processes in `/proc` instead.
- **SPS/PPS for any resolution.** The original only had SPS/PPS for 672x504, 240x180, 480x360,
  864x480 and 1280x720, and injected the 864x480 one for any other encoder size (e.g. 1080p),
  which breaks the stream. The fixed build generates them for the configured size, and prefers
  the encoder's own SPS/PPS whenever the encoder sends them. Playback uses the configured size
  too, instead of always 720p.
- **IPC hang.** The original's message sender thread sleeps while holding its lock, which can
  starve the main loop for long periods.

| Option | Effect |
|---|---|
| `--compat=y` | Build the original behavior, bugs included (also the stack overflow in the `ps` check). Memory leaks and out-of-bounds reads found while reversing stay fixed either way. |
| `--breakpad=y` | Writes minidumps to `/tmp/log/vm/` on crash, like the original. |

### 3. Run it on the robot

```sh
ssh $ROBOT 'cat > /data/video_monitor && chmod +x /data/video_monitor' < build-robot/linux/arm64/release/video_monitor
ssh $ROBOT /data/video_monitor -f /ava/conf/video_monitor/video_monitor.cfg
```

`-f` is optional; the config path must contain `video_monitor.cfg`. It refuses to start while
another `video_monitor` process is running.

What it reads at runtime:

| Path | Purpose |
|---|---|
| `/ava/conf/video_monitor/video_monitor.cfg` | Main config (JSON): `appid`, `video_filepath`, disk limits, `recorder_cfg_path`, ... |
| `recorder_cfg_path` (usually `/ava/conf/video_monitor/recorder.cfg`) | TRecorder config. `encoder_voutput_width` / `encoder_voutput_height` also set the injected SPS. |
| `/ava/conf/video_monitor/loudspeakerservice.json` | ALSA device for the intercom |
| `/mnt/private/certificate.bin` | Agora license. Startup retries every 5 s until `agora_rtc_license_verify` accepts it. |
| `/data/config/ava/iot.flag`, `/data/config/dmio/device.region` or `/data/config/miio/device.country` | Region, mapped to the Agora area code |
| `/data/config/dmio/device.conf` | `did=`, used as the Agora user id |

It logs to `/data/log/video_monitor.log`. For verbose logging, write `{"cam_mon": 1}` to
`/tmp/log/log_switch.json` before starting it.

### Building without a container

You need [xmake](https://xmake.io), `curl`, `tar`, `xz` and `ar` (binutils), on top of the robot
libraries from step 1. These steps do what the container image does.

**Toolchain.** Its glibc must not be newer than the robot's. For glibc 2.34, use Bootlin's
`2021.11-1` toolchain, which has GCC 10.3:

```sh
curl -LO https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/aarch64--glibc--stable-2021.11-1.tar.bz2
tar xjf aarch64--glibc--stable-2021.11-1.tar.bz2
TOOLCHAIN=$PWD/aarch64--glibc--stable-2021.11-1
```

**Headers.** The robot has libraries but no headers. Debian bullseye ships the same library
generations, so its arm64 `-dev` packages provide matching headers:

```sh
SYSROOT=$PWD/sysroot
mkdir -p "$SYSROOT/usr/include" debs && cd debs
MIRROR=https://deb.debian.org/debian
curl -sSfL "$MIRROR/dists/bullseye/main/binary-arm64/Packages.xz" | xz -d > Packages
for p in libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libssl-dev libcurl4-openssl-dev libasound2-dev; do
  f=$(awk -v p="$p" '$1=="Package:"{c=$2} c==p && $1=="Filename:"{print $2; exit}' Packages)
  curl -sSfLO "$MIRROR/$f"
  rm -rf x && mkdir x && (cd x && ar x "../${f##*/}" && tar xf data.tar.* ./usr/include && cp -a usr/include/. "$SYSROOT/usr/include/")
done
cd .. && rm -rf debs
```

If bullseye has moved off `deb.debian.org`, use `MIRROR=https://archive.debian.org/debian`.

**Build.** rapidjson is downloaded by xmake.

```sh
xmake f -p linux -a arm64 -m release --sdk="$TOOLCHAIN" --cross=aarch64-linux- \
        --vendor_root="$SYSROOT" -o build-robot
xmake
```

Optional sanity check: the binary must not need a newer glibc than the robot has.

```sh
readelf -V build-robot/linux/arm64/release/video_monitor | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
```

## Building and testing on a PC

For development, the vendor SDKs can be replaced by the stand-ins in `stubs/`, so the program
builds and runs on an x86_64 Linux machine. The third-party libraries come from xmake's
package manager and are built from source on the first build, which takes a few minutes. They
are pinned close to the robot's versions: OpenSSL 1.1.1, curl 7.87, ALSA 1.2.10. FFmpeg is 6.1
rather than the robot's 4.x, because the 4.4 package no longer builds with current binutils; the
code supports both. Nothing needs to be installed system-wide.

With the container image from [step 2](#2-build), pass `pc` (and any `xmake f` options after it):

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src \
           -v video_monitor-xmake:/opt/xmake-global video_monitor-build pc
```

Without a container, with xmake installed:

```sh
xmake f -p linux -a x86_64 -m debug -o build      # add --compat=y for the original behavior
xmake
```

Either way, the binary ends up in `build/linux/x86_64/debug/video_monitor`. The container's build
needs glibc 2.38 or newer on the machine that runs it.

`tools/host_test.sh <binary> [inband|first|strip] [busybox]` runs a full live-view session in a
bubblewrap sandbox (needs `bwrap`, `ffmpeg` with libx264, and `python3`). It does the following:
- Fakes `/data`, `/ava` and `/mnt/private`.
- Feeds an x264 test clip through the stub TRecorder.
- Sends a `monitor start` message.
- Decodes everything the stub Agora received.

The stubs can also be used directly:

| Variable / file | Purpose |
|---|---|
| `VM_STUB_H264=<file>` | Annex-B stream replayed as encoder output |
| `VM_STUB_STRIP_SPS=1` | Drop SPS/PPS from it (stock-encoder behavior) |
| `VM_STUB_AGORA_OUT=<file>` | Dump of every buffer passed to `agora_rtc_send_video_data` |
| `/tmp/videomonitor.socket.in` / `.out` | Messages to / from video_monitor, one per line |

## IPC protocol (ava ↔ video_monitor)

NN_PAIR socket, `ipc:///tmp/videomonitor.socket`. A request looks like this:

```json
{"method":"action","taskid":1,"value":"{\"operType\":\"monitor\",\"operation\":\"start\",\"session\":\"s1\",\"token\":\"...\",\"channelId\":\"...\",\"encryptionKey\":\"...\"}"}
```

The reply is the same envelope, with `result`/`status`/`df` added to the inner value.

- **`operType` values:** `monitor`, `playback`, `recordVideo`, `download`, `delete`, `upload`,
  `rename`, `get_properties`, `checkdisk`, `keep_alive`, `video` (reset), `intercom`,
  `restAliServer`.
- **Internal frames:** `$0...` asks for the speaker back. `$4?<path>` reloads the log config.
  `$1:<state>` reports a state change.
- **Sessions:** a session expires 60 s after its last message. When none are left, the stream
  is torn down.

## License

This project is licensed under the GNU General Public License version 3 (see [LICENSE](LICENSE)),
with the following additional permission.

### Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with any library
shipped as part of the firmware of the robot vacuum it runs on (such as the Agora RTC SDK, the
Allwinner media libraries, or that firmware's copy of OpenSSL), or a modified version of such a
library, containing parts covered by the terms of that library's license, the licensors of this
Program grant you additional permission to convey the resulting work. Corresponding Source for
a non-source form of such a combination does not need to include the source code of those
libraries.

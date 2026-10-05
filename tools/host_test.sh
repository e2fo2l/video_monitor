#!/bin/bash
# Runs a host build of video_monitor inside a bubblewrap sandbox that fakes the robot's
# filesystem (/data, /ava, /mnt/private), starts a live view over the stub IPC, replays an
# H.264 clip through the stub TRecorder, and checks whether what would be sent to Agora decodes.
#
#   tools/host_test.sh <video_monitor binary> [inband|first|strip] [busybox]
#
#   inband   encoder repeats SPS/PPS before every IDR (default)
#   first    encoder sends SPS/PPS only once, then bare IDRs
#   strip    encoder never sends SPS/PPS (video_monitor must inject its own)
#   busybox  truncate `ps` to 79 columns like stock firmware, so the faithful build starts
#
# Needs: bwrap, ffmpeg (with libx264), python3.
set -euo pipefail

BIN=$(readlink -f "$1")
MODE=${2:-inband}
PS_MODE=${3:-}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

mkdir -p "$W"/{data/config/ava,data/config/dmio,data/log,data/video,ava/conf/video_monitor,ava/bin,ava/shim,mnt/private,tmp}
printf dmiot > "$W/data/config/ava/iot.flag"
printf eu > "$W/data/config/dmio/device.region"
echo "did=123456789" > "$W/data/config/dmio/device.conf"
echo dummy > "$W/mnt/private/certificate.bin"
cat > "$W/ava/conf/video_monitor/video_monitor.cfg" <<'EOF'
{ "version": "1.0", "appid": "stub-appid", "camera_id": 0, "video_filepath": "/data/video/",
  "video_total_disk_space": 512, "video_file_size": 64, "video_least_file_size": 32,
  "video_least_disk_space": 16, "enable_catch_signal": 0,
  "recorder_cfg_path": "/ava/conf/video_monitor/recorder.cfg" }
EOF
printf 'encoder_voutput_width = 1280\nencoder_voutput_height = 720\n' > "$W/ava/conf/video_monitor/recorder.cfg"
cp "$BIN" "$W/ava/bin/video_monitor"
printf '#!/bin/sh\n/usr/bin/ps "$@" | cut -c1-79\n' > "$W/ava/shim/ps"
chmod +x "$W/ava/shim/ps"

ffmpeg -loglevel error -y -f lavfi -i testsrc=size=1280x720:rate=15 -t 4 -pix_fmt yuv420p \
    -c:v libx264 -profile:v main -g 15 -bsf:v h264_mp4toannexb -f h264 "$W/tmp/src.h264"
STRIP=""
case "$MODE" in
inband) ;;
strip) STRIP=1 ;;
first)
    python3 - "$W/tmp/src.h264" <<'EOF'
import re, sys
p = sys.argv[1]; d = open(p, 'rb').read()
idx = [m.start() for m in re.finditer(b'\x00\x00\x01', d)]
out, seen = bytearray(), 0
for k, s in enumerate(idx):
    b = s - 1 if s and d[s - 1] == 0 else s
    e = (idx[k + 1] - 1 if d[idx[k + 1] - 1] == 0 else idx[k + 1]) if k + 1 < len(idx) else len(d)
    if d[s + 3] & 0x1f in (7, 8):
        seen += 1
        if seen > 2:
            continue
    out += d[b:e]
open(p, 'wb').write(out)
EOF
    ;;
*) echo "unknown mode $MODE" >&2; exit 2 ;;
esac

cat > "$W/ava/run.sh" <<'EOF'
#!/bin/sh
/ava/bin/video_monitor -f /ava/conf/video_monitor/video_monitor.cfg &
i=0
while [ ! -p /tmp/videomonitor.socket.in ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
[ -p /tmp/videomonitor.socket.in ] || exit 0 # video_monitor exited during startup
sleep 1
echo '{"method":"action","taskid":1,"value":"{\"operType\":\"monitor\",\"operation\":\"start\",\"session\":\"s1\",\"token\":\"tok\",\"channelId\":\"chan\",\"encryptionKey\":\"key\"}"}' > /tmp/videomonitor.socket.in
sleep 5
kill %1 2>/dev/null
wait
EOF
chmod +x "$W/ava/run.sh"

PATH_IN=/usr/bin:/bin
[ "$PS_MODE" = busybox ] && PATH_IN=/ava/shim:$PATH_IN

set +e
timeout 30 bwrap --unshare-pid --as-pid-1 \
    --ro-bind /usr /usr --ro-bind /etc /etc \
    --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --symlink usr/sbin /sbin \
    --proc /proc --dev /dev --bind "$W/data" /data --bind "$W/ava" /ava --bind "$W/mnt" /mnt \
    --bind "$W/tmp" /tmp --setenv PATH "$PATH_IN" \
    ${LD_LIBRARY_PATH:+--setenv LD_LIBRARY_PATH "$LD_LIBRARY_PATH"} \
    --setenv VM_STUB_H264 /tmp/src.h264 --setenv VM_STUB_AGORA_OUT /tmp/agora.h264 \
    ${STRIP:+--setenv VM_STUB_STRIP_SPS 1} /ava/run.sh
set -e

echo "--- replies sent to ava:"
cat "$W/tmp/videomonitor.socket.out" 2>/dev/null || echo "(none)"
echo "--- log:"
grep -v 'lost heart' "$W/data/log/video_monitor.log" 2>/dev/null | tail -5 || true
OUT="$W/tmp/agora.h264"
if [ -s "$OUT" ]; then
    echo "--- sent to Agora: $(stat -c%s "$OUT") bytes, $(ffmpeg -v error -i "$OUT" -f null - 2>&1 | wc -l) decoder errors"
else
    echo "--- nothing was sent to Agora"
fi

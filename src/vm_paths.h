// Every robot filesystem path and IPC endpoint video_monitor uses, in one place.
// Where the original binary had a named global for a path, its name is noted.
#pragma once

namespace vmpath
{

// --- configuration ---------------------------------------------------------------------------
// Default main config; overridden by `-f <path>` (DAT_0053f7f8).
inline constexpr char VIDEO_MONITOR_CFG[] = "/ava/conf/video_monitor/video_monitor.cfg";
// ALSA playback device for the intercom (CONF_PATH_LP).
inline constexpr char LOUDSPEAKER_CFG[] = "/ava/conf/video_monitor/loudspeakerservice.json";
// ALSA capture device, unused (MIC_CONFIG_PATH).
inline constexpr char MICROPHONE_CFG[] = "/ava/conf/video_monitor/microphoneservice.json";

// --- device identity / region ----------------------------------------------------------------
// "did=<n>", used as the Agora user id (DEVICE_CONF_PATH).
inline constexpr char DEVICE_CONF[] = "/data/config/dmio/device.conf";
// "dmiot" or "miiot": selects which region file below is read.
inline constexpr char IOT_FLAG[] = "/data/config/ava/iot.flag";
inline constexpr char DMIO_DEVICE_REGION[] = "/data/config/dmio/device.region";
inline constexpr char MIIO_DEVICE_COUNTRY[] = "/data/config/miio/device.country";

// --- Agora -----------------------------------------------------------------------------------
inline constexpr char AGORA_CERTIFICATE[] = "/mnt/private/certificate.bin";
inline constexpr char AGORA_STORAGE_DIR[] = "/data/image/upload/";

// --- IPC with ava ----------------------------------------------------------------------------
inline constexpr char IPC_SOCKET[] = "ipc:///tmp/videomonitor.socket"; // SOCKET_PATH

// --- logging / crash dumps -------------------------------------------------------------------
inline constexpr char LOG_FILE[] = "/data/log/video_monitor.log";
inline constexpr char LOG_FILE_BAK[] = "/data/log/video_monitor.log.bak";
// {"cam_mon": 1} enables verbose logging.
inline constexpr char LOG_SWITCH_CFG[] = "/tmp/log/log_switch.json";
inline constexpr char CRASH_DUMP_DIR[] = "/tmp/log/vm/";

// --- debug dumps (enable_save_video_h264 / enable_save_app_audio in the main config) ---------
inline constexpr char DEBUG_H264_DUMP[] = "/data/log/send_h264.h264";
inline constexpr char DEBUG_AUDIO_DUMP[] = "/data/test/rcv_audio.pcm";

// --- cloud upload ----------------------------------------------------------------------------
inline constexpr char CA_CERT_DIR[] = "/etc/ssl/certs";

} // namespace vmpath

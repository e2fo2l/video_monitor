// Agora RTSA (lite) C API, as used by video_monitor.
// Reconstructed from call sites in the binary; layouts match the offsets observed in the
// aarch64 code. Only the subset video_monitor touches is declared.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C"
{

  typedef enum
  {
    AUDIO_DATA_TYPE_OPUS = 1,
    AUDIO_DATA_TYPE_PCM = 100,
  } audio_data_type_e;

  typedef enum
  {
    VIDEO_DATA_TYPE_YUV420 = 0,
    VIDEO_DATA_TYPE_H264 = 2,
  } video_data_type_e;

  typedef enum
  {
    VIDEO_FRAME_KEY = 1,
    VIDEO_FRAME_DELTA = 2,
    VIDEO_FRAME_AUTO_DETECT = 3,
  } video_frame_type_e;

  typedef enum
  {
    VIDEO_FRAME_RATE_FPS_15 = 15,
  } video_frame_rate_e;

  // 0x80 bytes; slot order verified against MonitorAgoraService::EventHandlerinit (0x464898).
  typedef struct
  {
    void (*on_join_channel_success)(const char* channel, int elapsed_ms);               // +0x00
    void (*on_connection_lost)(const char* channel);                                    // +0x08
    void (*on_rejoin_channel_success)(const char* channel, int elapsed_ms);             // +0x10
    void (*on_error)(const char* channel, int code, const char* msg);                   // +0x18
    void (*on_user_joined)(const char* channel, uint32_t uid, int elapsed_ms);          // +0x20
    void (*on_user_offline)(const char* channel, uint32_t uid, int reason);             // +0x28
    void (*on_user_mute_audio)(const char* channel, uint32_t uid, int muted);           // +0x30
    void (*on_user_mute_video)(const char* channel, uint32_t uid, int muted);           // +0x38
    void (*on_key_frame_gen_req)(const char* channel, uint32_t uid, uint8_t stream_id); // +0x40
    void (*on_audio_data)(const char* channel, uint32_t uid, uint16_t sent_ts, audio_data_type_e data_type,
                          const void* data, size_t len); // +0x48
    void (*on_mixed_audio_data)(const char* channel, audio_data_type_e data_type, const void* data,
                                size_t len); // +0x50
    void (*on_video_data)(const char* channel, uint32_t uid, uint16_t sent_ts, video_data_type_e data_type,
                          uint8_t stream_id, int is_key_frame, const void* data, size_t len); // +0x58
    void (*on_target_bitrate_changed)(const char* channel, uint32_t target_bps);              // +0x60
    void* reserved[3];                                                                        // +0x68
  } agora_rtc_event_handler_t;

  // 0x28 bytes, passed to agora_rtc_init.
  typedef struct
  {
    uint32_t area_code;      // +0x00  AREA_CODE_* bitmask, 0xFFFFFFFF = global
    uint8_t reserved[0x1c];  // +0x04  zeroed by video_monitor
    const char* storage_dir; // +0x20  "/data/image/upload/"
  } rtc_service_option_t;

  typedef struct
  {
    int audio_codec_type; // +0x04 (inside rtc_channel_options_t)
    int pcm_sample_rate;
    int pcm_channel_num;
  } audio_codec_option_t;

  // 0x14 bytes, passed to agora_rtc_join_channel.
  typedef struct
  {
    bool auto_subscribe_audio;            // +0x00
    bool auto_subscribe_video;            // +0x01
    audio_codec_option_t audio_codec_opt; // +0x04
    bool enable_aut_encryption;           // +0x10
  } rtc_channel_options_t;

  typedef struct
  {
    video_data_type_e data_type;   // +0x00
    video_frame_type_e frame_type; // +0x04
    video_frame_rate_e frame_rate; // +0x08
  } video_frame_info_t;

  int agora_rtc_license_verify(const char* certificate, int certificate_len, const char* credential,
                               int credential_len);
  int agora_rtc_init(const char* app_id, const agora_rtc_event_handler_t* event_handler, rtc_service_option_t* option);
  int agora_rtc_fini(void);
  int agora_rtc_set_log_level(int level);
  int agora_rtc_config_log(int size_per_file, int max_file_count);
  int agora_rtc_set_params(const char* params);
  // video_monitor passes the user id as a decimal string (the device DID).
  int agora_rtc_join_channel(const char* channel, const char* uid, const char* token, size_t token_len,
                             rtc_channel_options_t* options);
  int agora_rtc_leave_channel(const char* channel);
  int agora_rtc_send_video_data(const char* channel, uint8_t stream_id, const void* data, size_t len,
                                video_frame_info_t* info);
  const char* agora_rtc_err_2_str(int err);

} // extern "C"

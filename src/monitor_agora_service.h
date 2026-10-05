// MonitorAgoraService — owns the Agora RTC session the H.264 stream is pushed into.
#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <queue>
#include <string>

#include <vendor/agora_rtc_api.h>

#include "monitor_common.h"

class MonitorAgoraService
{
public:
  static MonitorAgoraService& GetPtr();

  bool MonitorAgoraInit();
  int MonitorAgoraUinit();
  bool MonitorAgoraConnect(const std::string& token, const std::string& channelId, const std::string& encryptionKey);
  bool MonitorAgoraDisConnect();

  bool getQueueH264Buffer(H264Buffer& out);
  void setQueueH264Buffer(const H264Buffer& buf);
  void SetStreamCount(long count);

  bool GetQueueAudio(AudioDataFromAgora& out);
  size_t GetQueueAudioSize();
  void AddQueueAudio(const AudioDataFromAgora& data);
  void CleanQueueAudio();

  void StartSendAudio() {}
  void StopSendAudio() {}
  void SendAudioToAgoraThread() {}

  static std::atomic<bool> m_bConnectSuccess;
  static std::atomic<bool> m_bSendFirst; // wait for a key frame before sending

private:
  MonitorAgoraService();

  bool VerifyLicense(const std::string& path);
  bool GetAreaCode(unsigned& areaCode);
  static void EventHandlerinit(agora_rtc_event_handler_t* h);
  static std::string GetEncryptionMsg(bool enable, const std::string& mode, const std::string& key);
  void SendVideoToAgoraThread();
  bool WaitJoinChannel(unsigned long seconds);

  static void _on_join_channel_success(const char* channel, int elapsed);
  static void __on_rejoin_channel_success(const char* channel, int elapsed);
  static void __on_connection_lost(const char* channel);
  static void __on_user_joined(const char*, uint32_t, int) {}
  static void __on_user_offline(const char*, uint32_t, int) {}
  static void __on_user_mute_audio(const char*, uint32_t, int) {}
  static void __on_user_mute_video(const char*, uint32_t, int) {}
  static void __on_target_bitrate_changed(const char*, uint32_t) {}
  static void __on_key_frame_gen_req(const char*, uint32_t, uint8_t) {}
  static void __on_video_data(const char*, uint32_t, uint16_t, video_data_type_e, uint8_t, int, const void*, size_t) {}
  static void __on_error(const char* channel, int code, const char* msg);
  static void __on_audio_data(const char* channel, uint32_t uid, uint16_t sentTs, audio_data_type_e type,
                              const void* data, size_t len);

  agora_rtc_event_handler_t m_eventHandler{};  // +0x000
  std::string m_channel;                       // +0x080
  std::atomic<bool> m_bExit{false};            // +0x0a0
  std::mutex m_h264Mutex;                      // +0x0a8
  std::queue<H264Buffer> m_h264Queue;          // +0x0d8
  std::queue<AudioDataFromAgora> m_audioQueue; // +0x128
  std::mutex m_audioMutex;                     // +0x178
  std::map<std::string, unsigned> m_areaCodes; // +0x1b0
  long m_streamCount = 0;                      // +0x1e0
};

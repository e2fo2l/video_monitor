// MonitorAudio — two-way "intercom": app -> robot speaker via Agora audio callbacks.
#pragma once

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>

#include "monitor_common.h"
#include "monitor_loudspeaker.h"

class MonitorAudio
{
public:
  static MonitorAudio& GetPtr();

  uint16_t InitMicAndSpeaker();
  void CloseAudio(bool byVm);
  bool IsAudioOpen() const { return m_bAudioOpen; }
  void SendAudio() {}
  void InterTalkControl(MonitorJson& value, MonitorJson& msg);
  void InterruptSpeaker(const std::string& rawMsg);
  std::string getAudioSession() const { return m_session; }
  void setAudioSession(const std::string& s) { m_session = s; }

private:
  MonitorAudio() = default;

  void WaitEnoughAudioData();
  bool GetCacheData(AudioDataFromAgora& out);
  bool ResetCacheData(const AudioDataFromAgora* data);
  void RecvAudio();
  void GetTimeCountJson(MonitorJson& j);
  void ThreadCheckAudioPlayOver();
  bool WaitSpeakerControl();

  std::atomic<bool> m_bAudioOpen{false};     // +0x00
  std::atomic<bool> m_bRecvThreadRun{false}; // +0x01
  std::atomic<bool> m_bReadySent{false};     // +0x02
  std::string m_session;                     // +0x08
  std::atomic<bool> m_bClosedByVm{false};    // +0x28
  time_t m_talkStart = 0;                    // +0x30
  time_t m_talkEnd = 0;                      // +0x38
  int m_phone = -1;                          // +0x40
  MonitorLoudspeaker m_speaker;              // +0x48
  std::atomic<bool> m_bInterrupt{false};     // +0x90 ava wants the speaker
  std::atomic<bool> m_bInterrupting{false};  // +0x91
  std::atomic<bool> m_bWaitPlayOver{false};  // +0x92
  std::mutex m_interruptMutex;               // +0x98
  std::mutex m_recvMutex;                    // +0xc8
};

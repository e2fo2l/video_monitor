#include "monitor_audio.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

#include <unistd.h>

#include "monitor_agora_service.h"
#include "monitor_common.h"
#include "monitor_json.h"
#include "ms_log.h"
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"
#include "util/thread_name.h"

namespace
{
// Last packet replayed while the network stalls, to avoid speaker underruns.
AudioDataFromAgora g_cache;              // DAT_0053f6c0
int g_cacheRepeat = 0;                   // DAT_0053f6d0
int g_cacheRound = 0;                    // DAT_0053f6d4
std::atomic<bool> g_speakerReady{false}; // DAT_0053f6d8
bool g_interruptLogged = false;          // DAT_0053f6d9
} // namespace

// Static-init singleton (0x474c14).
MonitorAudio& MonitorAudio::GetPtr()
{
  // Never destroyed, like the original, so no destructor runs at exit while the worker
  // threads still use it.
  static auto* instance = new MonitorAudio(); // NOLINT(cppcoreguidelines-owning-memory)
  return *instance;
}

// 0x472210
uint16_t MonitorAudio::InitMicAndSpeaker()
{
  if (m_speaker.LoudspeakerInit())
    return VM_OK;
  LOG_ERR << "[InterTalk]LoudspeakerInit failed";
  m_speaker.LoudspeakerUninit();
  return VM_ERR_SPEAKER_INIT;
}

// 0x472334
void MonitorAudio::CloseAudio(bool byVm)
{
  m_bClosedByVm = byVm;
  const MONITOR_SYS_STATE s = getMonitorState();
  if (s == MONITOR_TALK_START)
    setMonitorState(MONITOR_VIDEO_START);
  else if (s == MONITOR_TALK_RECORD_START)
    setMonitorState(MONITOR_RECORD_START);
  else
    LOG_ERR << "[InterTalk]close with invalid state:" << MonitorStateToStr(s);
  m_bAudioOpen = false;

  if (m_phone == -1)
    return;
  MonitorJson j;
  auto& a = j.GetAllocator();
  j.AddMember("operType", "intercom", a);
  j.AddMember("operation", "end", a);
  j.AddMember("session", "closeAudio", a);
  j.AddMember("phone", m_phone, a);
  j.AddMember("result", 0, a);
  j.AddMember("status", static_cast<int>(getMonitorState()), a);
  GetTimeCountJson(j);
  ResponseToApp(j);
}

// 0x472654 — up to 4 s for at least 8 packets of jitter buffer.
void MonitorAudio::WaitEnoughAudioData()
{
  LOG_INFO << "[WaitEnoughAudioData] begin wait!";
  for (int tries = 400;;)
  {
    const size_t n = MonitorAgoraService::GetPtr().GetQueueAudioSize();
    if (n > 7)
    {
      LOG_INFO << "[WaitEnoughAudioData] F cache sieze:" << n;
      return;
    }
    if (--tries < 1)
      break;
    usleep(10000);
  }
  LOG_ERR << "[WaitEnoughAudioData] wait too long time\xef\xbc\x8cplease check net!";
}

// 0x472944 — replays the last packet up to 3 times with growing gaps (50, 65, 80 ticks).
bool MonitorAudio::GetCacheData(AudioDataFromAgora& out)
{
  if (!g_cache.audioBuffer)
    return false;
  if (g_cacheRound >= 3)
  {
    free(g_cache.audioBuffer); // NOLINT(cppcoreguidelines-owning-memory): malloc'd packet from the Agora queue
    g_cache.audioBuffer = nullptr;
    return false;
  }
  if (++g_cacheRepeat > (g_cacheRound * 15) + 50)
  {
    ++g_cacheRound;
    out = g_cache;
    return true;
  }
  return false;
}

// 0x472a38 — takes ownership of `data`.
bool MonitorAudio::ResetCacheData(const AudioDataFromAgora* data)
{
  if (g_cache.audioBuffer)
  {
    free(g_cache.audioBuffer); // NOLINT(cppcoreguidelines-owning-memory): malloc'd packet from the Agora queue
    g_cache.audioBuffer = nullptr;
  }
  if (data)
    g_cache = *data;
  g_cacheRound = 0;
  g_cacheRepeat = 0;
  return true;
}

// 0x472abc — speaker feeding thread.
void MonitorAudio::RecvAudio()
{
  SetThreadName("RecvAudio");
  if (!WaitSpeakerControl())
  {
    m_bRecvThreadRun = false;
    return;
  }
  WaitEnoughAudioData();
  m_bReadySent = true;
  ResponseToApp(TASK_INTERCOM, "intercom", "ready");

  while (m_bAudioOpen)
  {
    usleep(1000);
    std::scoped_lock const lock(m_interruptMutex);
    if (m_bInterrupt)
    {
      if (!g_interruptLogged)
      {
        LOG_INFO << "[RecvAudio]audio interrupting!";
        g_interruptLogged = true;
      }
      ResetCacheData(nullptr);
      usleep(100000);
      continue;
    }
    g_interruptLogged = false;
    AudioDataFromAgora pkt;
    if (MonitorAgoraService::GetPtr().GetQueueAudio(pkt))
      ResetCacheData(&pkt);
    else if (!GetCacheData(pkt))
      continue;
    m_speaker.LoudspeakerWriteData(pkt);
  }

  std::scoped_lock const lock(m_recvMutex);
  m_bRecvThreadRun = false;
  m_bReadySent = false;
  if (!m_speaker.LoudspeakerUninit())
    LOG_ERR << "[RecvAudio]LoudspeakerUninit failed";
  LOG_INFO << "[RecvAudio]thread receiveAudio  exit";
  MonitorAgoraService::GetPtr().CleanQueueAudio();
  ResetCacheData(nullptr);
  m_bWaitPlayOver = false;
  if (m_bClosedByVm)
  {
    LOG_INFO << "[RecvAudio]vm end!";
  }
  else
  {
    ResponseToApp(TASK_INTERCOM, "intercom", "stop");
    LOG_INFO << "[RecvAudio]send stop!!";
  }
}

// 0x47328c — operType "intercom": {"operation":"start"|"end","session":..,"phone":..}
void MonitorAudio::InterTalkControl(MonitorJson& value, MonitorJson& msg)
{
  std::string operation;
  std::string session;
  if (!GetMsgParam(value, msg, "operation", "[GetProp]", operation, false))
    return;
  if (!GetMsgParam(value, msg, "session", "[GetProp]", session, false))
    return;
  GetMsgParam(value, msg, "phone", m_phone, false);

  const MONITOR_SYS_STATE state = getMonitorState();
  uint16_t result = VM_OK;

  if (operation == "start")
  {
    m_bClosedByVm = false;
    if (m_bAudioOpen)
    {
      LOG_ERR << "[interTalk]already opended!!";
      return; // NOTE(orig): no reply is sent
    }
    if (state != MONITOR_VIDEO_START && state != MONITOR_RECORD_START)
    {
      LOG_ERR << "[interTalk]start with invalid state:" << MonitorStateToStr(state);
      return; // NOTE(orig): no reply is sent
    }
    if (m_phone != -1)
      m_talkStart = time(nullptr);
    m_bAudioOpen = true;
    setMonitorState(state == MONITOR_VIDEO_START ? MONITOR_TALK_START : MONITOR_TALK_RECORD_START);

    if (m_bRecvThreadRun)
    {
      LOG_INFO << "[InterTalk] recv thread still run!";
    }
    else
    {
      std::unique_lock lock(m_recvMutex);
      MonitorAgoraService::GetPtr().CleanQueueAudio();
      g_speakerReady = (InitMicAndSpeaker() == VM_OK);
      if (!g_speakerReady)
        LOG_ERR << "[InterTalk]InitMicAndSpeaker failed!";
      m_bRecvThreadRun = true;
      lock.unlock();
      std::thread(&MonitorAudio::RecvAudio, this).detach();
    }
    LOG_INFO << "[InterTalk]start interTalkControl success";
    setAudioSession(session);
  }
  else if (operation == "end")
  {
    bool ok = true;
    if (state == MONITOR_TALK_START)
    {
      setMonitorState(MONITOR_VIDEO_START);
    }
    else if (state == MONITOR_TALK_RECORD_START)
    {
      setMonitorState(MONITOR_RECORD_START);
    }
    else
    {
      LOG_ERR << "[InterTalk]close with invalid state:" << MonitorStateToStr(state);
      ok = false;
    }
    m_bAudioOpen = false;
    GetTimeCountJson(value);
    if (!ok)
    {
      LOG_ERR << "[interTalk]end with invalid state:" << MonitorStateToStr(state);
      return;
    }
  }
  else
  {
    LOG_ERR << "[InterTalk]invalid operation:" << operation;
    result = VM_ERR_INVALID_OPERATION;
  }
  ResponseToApp(value, msg, result);
}

// 0x473ee0 — "$0..." from ava: another process (voice prompts) needs the speaker.
void MonitorAudio::InterruptSpeaker(const std::string& rawMsg)
{
  if (!m_bAudioOpen || !m_bReadySent)
  {
    ResponseToAva(rawMsg);
    return;
  }
  if (m_bInterrupting)
  {
    ResponseToAva(rawMsg);
    LOG_ERR << "[InterruptSpeaker]Interrupt double kill!";
    return;
  }
  std::unique_lock lock(m_interruptMutex);
  m_bInterrupt = true;
  lock.unlock();
  if (!m_speaker.LoudspeakerUninit())
    LOG_ERR << "[InterruptSpeaker]LoudspeakerUninit failed";
  LOG_INFO << "[InterruptSpeaker] success\xef\xbc\x81";
  m_bInterrupting = true;
  std::thread(&MonitorAudio::ThreadCheckAudioPlayOver, this).detach();
  ResponseToAva(rawMsg);
}

// 0x4742bc — "record": [phone, start, end, duration]
void MonitorAudio::GetTimeCountJson(MonitorJson& j)
{
  if (m_phone == -1)
    return;
  auto& a = j.GetAllocator();
  m_talkEnd = time(nullptr);
  rapidjson::Value arr(rapidjson::kArrayType);
  arr.PushBack(rapidjson::Value().SetInt(m_phone), a);
  arr.PushBack(rapidjson::Value().SetInt64(m_talkStart), a);
  arr.PushBack(rapidjson::Value().SetInt64(m_talkEnd), a);
  arr.PushBack(rapidjson::Value().SetInt64(m_talkEnd - m_talkStart), a);
  j.AddMember("record", arr, a);
  m_phone = -1;
}

// 0x474484 — waits for the other process to release the speaker, then re-acquires it.
void MonitorAudio::ThreadCheckAudioPlayOver()
{
  m_bWaitPlayOver = true;
  std::this_thread::sleep_for(std::chrono::seconds(2));
  while (m_bWaitPlayOver && !m_speaker.TryControlLoudSpeaker())
    usleep(100000);
  while (m_bAudioOpen)
  {
    if (m_speaker.LoudspeakerInit())
    {
      LOG_INFO << "[ThreadCheckAudioPlayOver] LoudspeakerInit success\xef\xbc\x81";
      ResponseToAva(MakeInterMsg(INTER_MSG_SPEAKER_RESUMED, ""));
      break;
    }
    LOG_ERR << "[ThreadCheckAudioPlayOver] LoudspeakerInit failed";
    m_speaker.LoudspeakerUninit();
    usleep(100000);
  }
  m_bInterrupting = false;
  m_bInterrupt = false;
}

// 0x474798 — retries speaker init every 0.5 s for up to 10 s.
bool MonitorAudio::WaitSpeakerControl()
{
  if (g_speakerReady)
    return true;
  LOG_INFO << "[WaitSpeaker]Begin !";
  const long start = GetCpuTime();
  for (;;)
  {
    usleep(500000);
    if (!m_bAudioOpen)
    {
      LOG_ERR << "[WaitSpeaker]intercom closed!";
      return false;
    }
    if (InitMicAndSpeaker() == VM_OK)
    {
      g_speakerReady = true;
      LOG_INFO << "[WaitSpeaker]success!";
      return true;
    }
    if (GetCpuTime() - start > 10000000)
      break;
  }
  LOG_ERR << "[WaitSpeaker]out of time!";
  return false;
}

#include "monitor_msg_center.h"

#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include "monitor_audio.h"
#include "monitor_common.h"
#include "monitor_json.h"
#include "monitor_video.h"
#include "ms_log.h"
#include "util/thread_name.h"
#include "vendor/nnxx.h"
#include "vm_paths.h"

// Static-init singleton (0x48d6a0).
MonitorMsgCenter& MonitorMsgCenter::GetPtr()
{
  // Never destroyed, like the original, so no destructor runs at exit while the worker
  // threads still use it.
  static auto* instance = new MonitorMsgCenter(); // NOLINT(cppcoreguidelines-owning-memory)
  return *instance;
}

// 0x48d1a4
bool MonitorMsgCenter::Init()
{
  if (!InitSocket())
    return false;
  RegMsgTypeFunc();
  std::thread(&MonitorMsgCenter::ThreadSendMsg, this).detach();
  return true;
}

// 0x48cff4 — NOTE(orig): connect() errors are ignored, this always succeeds.
bool MonitorMsgCenter::InitSocket()
{
  m_socket = nnxx::socket(nnxx::SP, nnxx::PAIR);
  m_socket.connect(vmpath::IPC_SOCKET);
  return true;
}

// 0x48c930
void MonitorMsgCenter::RegMsgTypeFunc()
{
  MonitorVideo& v = MonitorVideo::GetPtr();
  MonitorAudio& a = MonitorAudio::GetPtr();
  auto bindV = [&v](void (MonitorVideo::*fn)(MonitorJson&, MonitorJson&))
  { return [&v, fn](MonitorJson& value, MonitorJson& msg) { (v.*fn)(value, msg); }; };
  m_handlers = {
      {"monitor", bindV(&MonitorVideo::MonitorVideoControl)},
      {"playback", bindV(&MonitorVideo::PlayBackVideoControl)},
      {"recordVideo", bindV(&MonitorVideo::RecordVideoControl)},
      {"download", bindV(&MonitorVideo::DownloadVideoControl)},
      {"delete", bindV(&MonitorVideo::DeleteVideoControl)},
      {"upload", bindV(&MonitorVideo::UploadVideoControl)},
      {"rename", bindV(&MonitorVideo::RenameVideoControl)},
      {"get_properties", bindV(&MonitorVideo::GetPropVideoControl)},
      {"checkdisk", bindV(&MonitorVideo::CheckDiskVideoControl)},
      {"keep_alive", bindV(&MonitorVideo::KeepAliveVideoControl)},
      {"video", bindV(&MonitorVideo::ResetVideoControl)},
      {"intercom", [&a](MonitorJson& value, MonitorJson& msg) { a.InterTalkControl(value, msg); }},
      {"restAliServer", bindV(&MonitorVideo::RestAliService)},
  };
}

// 0x48bed8 — polled every 1 ms from main().
void MonitorMsgCenter::HandleMsg()
{
  nnxx::message const m = m_socket.recv(nnxx::DONTWAIT);
  if (m.empty())
    return;
  ResetHeartBeatSwitch();
  ProcMsgs(m);
}

// 0x48bf64
void MonitorMsgCenter::ProcMsgs(const nnxx::message& m)
{
  MonitorJson msg;
  MonitorJson value;
  const std::string raw(m.begin(), m.end());
  LOG_INFO << "[MsgCenter]Rcv msg:" << raw;
  if (ProcInternalMsg(raw))
    return;

  if (msg.Parse(raw.c_str()).HasParseError())
  {
    LOG_ERR << "[MsgCenter]json prase msg error!!";
    return;
  }
  const auto valueIt = msg.FindMember("value");
  if (valueIt == msg.MemberEnd() || !valueIt->value.IsString())
  {
    LOG_ERR << "[MsgCenter]Msg has no member value or value is not string!!";
    return;
  }
  if (value.Parse(valueIt->value.GetString()).HasParseError())
  {
    LOG_ERR << "[MsgCenter]json prase msg value error!!";
    return;
  }
  const auto operTypeIt = value.FindMember("operType");
  if (operTypeIt == value.MemberEnd() || !operTypeIt->value.IsString())
  {
    LOG_ERR << "[MsgCenter]value has no member operType or operType is not string";
    return;
  }
  const auto sessionIt = value.FindMember("session");
  if (sessionIt == value.MemberEnd() || !sessionIt->value.IsString())
  {
    LOG_ERR << "[MsgCenter]value has no member session or session is not string";
    return;
  }
  const std::string session = sessionIt->value.GetString();
  const std::string operType = operTypeIt->value.GetString();
  UpdateSession(session);

  const auto it = m_handlers.find(operType);
  if (it == m_handlers.end())
  {
    LOG_ERR << "[MsgCenter]operType not reg:" << operType;
    ResponseToApp(value, msg, VM_ERR_UNKNOWN_OPERTYPE);
    return;
  }
  it->second(value, msg);
}

// 0x48d574 — "$0..." interrupt speaker, "$4?<path>" reload log config.
bool MonitorMsgCenter::ProcInternalMsg(const std::string& msg)
{
  if (msg.empty() || msg.at(0) != '$')
    return false;
  const int id = msg.at(1) - '0';
  if (id == INTER_MSG_INTERRUPT_SPEAKER)
    MonitorAudio::GetPtr().InterruptSpeaker(msg);
  else if (id == INTER_MSG_RELOAD_LOG_CFG)
    MSLog::getInstance().GetConfigFromFile(std::string(&msg.at(3)));
  return true;
}

// 0x48d258 — bounded at 128 pending messages (oldest dropped).
void MonitorMsgCenter::SendMsg(const std::string& msg)
{
  if (msg.empty())
    return;
  std::scoped_lock const lock(m_sendMutex);
  if (m_sendQueue.size() >= 128)
  {
    m_sendQueue.pop();
    LOG_ERR << "[MsgCenter]msg list full:" << 128;
  }
  m_sendQueue.push(msg);
}

// 0x48d3dc
void MonitorMsgCenter::ThreadSendMsg()
{
  SetThreadName("VMS_send_msg");
  for (;;)
  {
    std::unique_lock lock(m_sendMutex);
    if (m_sendQueue.empty())
    {
#if !VM_COMPAT
      lock.unlock();
#endif
      // NOTE(orig): sleeps while holding the lock and re-locks immediately, so SendMsg()
      // callers (including the main loop) can starve for a long time -> sporadic hangs.
      usleep(1000);
      continue;
    }
    const int sent = m_socket.send(nnxx::make_message(m_sendQueue.front()), 0);
    m_sendQueue.pop();
    lock.unlock();
    if (sent < 1)
      LOG_ERR << "[MsgCenter]send fail!";
  }
}

// 0x48ce48 — every app session timed out.
void MonitorMsgCenter::ProcExit()
{
  LOG_ERR << "[MsgCenter] lost heart beaten";
  MonitorVideo::GetPtr().CloseMonitor("[lost heart beaten]");
  ResponseOpState("monitor");
}

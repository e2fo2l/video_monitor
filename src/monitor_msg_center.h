// MonitorMsgCenter — nanomsg NN_PAIR link to the "ava" daemon (ipc:///tmp/videomonitor.socket).
//
// Inbound frames are either internal "$<id>..." strings or JSON envelopes:
//     {"method": ..., "taskid": <int>, "value": "<json string>"}
// where the inner value carries {"operType": <handler>, "session": <id>, ...}.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>

#include <vendor/nnxx.h>

#include "monitor_json.h"

class MonitorMsgCenter
{
public:
  using Handler = std::function<void(MonitorJson& value, MonitorJson& msg)>;

  static MonitorMsgCenter& GetPtr();

  bool Init();
  void HandleMsg();
  void SendMsg(const std::string& msg);
  static void ProcExit();

private:
  MonitorMsgCenter() = default;

  bool InitSocket();
  void RegMsgTypeFunc();
  void ProcMsgs(const nnxx::message& m);
  bool ProcInternalMsg(const std::string& msg);
  [[noreturn]] void ThreadSendMsg();

  std::map<std::string, Handler> m_handlers; // +0x00
  nnxx::socket m_socket;                     // +0x30
  std::queue<std::string> m_sendQueue;       // +0x38
  std::mutex m_sendMutex;                    // +0x88
};

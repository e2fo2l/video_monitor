// video_monitor entry point.
//
//   video_monitor [-f <path/to/video_monitor.cfg>]
//
// Lifecycle: single-instance check -> connect to ava (nanomsg) -> parse cfg -> Agora init
// (retried forever) -> announce "monitor" ready -> 1 kHz poll loop.
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

#include "monitor_agora_service.h"
#include "monitor_common.h"
#include "monitor_msg_center.h"
#include "ms_log.h"
#include "vm_paths.h"

namespace get_monitor_version
{
namespace
{
// 0x497ac4
[[maybe_unused]] const char* GetMonitorVersion() { return "commit-r2212_0041release-0-g8da91c04"; }
} // namespace
} // namespace get_monitor_version

// 0x487368
int main(int argc, char** argv)
{
  // How many "video_monitor" substrings our own `ps` line is expected to contain.
  int selfCount = 1;
  if (argc > 2 && std::string(argv[1]) == "-f")
  {
    const std::string cfg = argv[2];
    if (cfg.find("video_monitor.cfg") == std::string::npos)
    {
      std::cerr << "Not correct cfg, please check the name!!\n";
      return -1;
    }
    VmSetConfigPath(cfg);
    ++selfCount;
  }

  if (CheckVmIsAlreadyRun(selfCount))
  {
    std::cerr << "video_monitor is Running, Please Check\n";
    return -1;
  }

  MonitorMsgCenter& mc = MonitorMsgCenter::GetPtr();
  if (!mc.Init())
  {
    LOG_ERR << "[main] VMS MonitorMsgCenter init failed!";
    return -1;
  }
  if (!InitSysConf())
  {
    LOG_ERR << "[main] VMS InitSysConf failed!";
    return -1;
  }

  // Blocks forever if /mnt/private/certificate.bin, iot.flag or the region file is missing.
  while (!MonitorAgoraService::GetPtr().MonitorAgoraInit())
    std::this_thread::sleep_for(std::chrono::seconds(5));

  if (GetSysConf().enable_catch_signal != 0)
    mkdir(vmpath::CRASH_DUMP_DIR, 0700);

  ResetAliveTimer();
  ResponseToApp("properties_changed", TASK_MONITOR, "properties_changed", "monitor", 0, 0, 0);
  ResponseToAva(MakeInterMsg(INTER_MSG_MONITOR_STATE, std::to_string(getMonitorState())));

  for (;;)
  {
    usleep(1000);
    if (CheckTimeoutDead())
      MonitorMsgCenter::ProcExit();
    mc.HandleMsg();
  }
}

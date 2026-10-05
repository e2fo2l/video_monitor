// Crash handling: breakpad minidumps to /tmp/log/vm/ plus a backtrace in the log.
#include <cstdlib>
#include <fstream>
#include <string>

#include <execinfo.h>
#include <sys/types.h>
#include <unistd.h>

#include "ms_log.h"

#if VM_COMPAT
#include <cstdio>
#endif

#if VM_WITH_BREAKPAD
#include <client/linux/handler/exception_handler.h>

#include "monitor_common.h"
#include "vm_paths.h"
#endif

namespace
{
// 0x497168
void get_thread_pid(bool withName)
{
  const pid_t pid = getpid();
  const pid_t tid = gettid();
  LOG_ERR << "[logTrace]Father pid: " << pid << "child pid: " << tid;
  if (!withName)
    return;
  const std::string commPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/comm";
  std::string name;
#if VM_COMPAT
  // The original ran `cat <comm>` twice: through popen (0x49793c, logExecuteCmd) to read the
  // name, then through system() to echo it to stdout.
  const std::string cmd = "cat " + commPath;
  // NOLINTNEXTLINE(cert-env33-c,bugprone-command-processor): faithful to the original
  if (FILE* fp = popen(cmd.c_str(), "r"))
  {
    char buf[0x40] = {};
    if (fgets(buf, sizeof buf, fp) != nullptr)
      name = buf;
    pclose(fp);
  }
  // NOLINTNEXTLINE(cert-env33-c,bugprone-command-processor,concurrency-mt-unsafe): faithful to the original
  std::system(cmd.c_str());
#else
  // NOTE(orig): reads the thread name directly instead of starting two shells from a crash
  // handler.
  std::ifstream comm(commPath);
  std::getline(comm, name);
  name += '\n'; // the original logged the name with cat's trailing newline
#endif
  LOG_ERR << "[logTrace]threadName, father is: " << pid << "child pid: " << tid << "(" << name << ")";
}

// 0x49746c
[[maybe_unused]] int logGetBackTrace(void** frames, int max)
{
  constexpr int DEFAULT_CALLS = 4;
  const int n = backtrace(frames, max);
  if (n < DEFAULT_CALLS)
    LOG_ERR << "[logGetBackTrace]numsOfCall= " << n << "DEFAULT_CALLS= " << DEFAULT_CALLS;
  LOG_ERR << "[logGetBackTrace]obtained= " << n << " stack frames, start ****************";
  char** syms = backtrace_symbols(frames, n);
  for (int i = 0; i < n; ++i)
    LOG_ERR << "[logGetBackTrace]string: " << (syms ? syms[i] : "?");
  LOG_ERR << "*************backtrace end ****************";
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): backtrace_symbols() returns one malloc'd block
  free(static_cast<void*>(syms));
  get_thread_pid(true);
  return n;
}

#if VM_WITH_BREAKPAD
// 0x4871d8
bool dumpCallback(const google_breakpad::MinidumpDescriptor& d, void* /*context*/, bool succeeded)
{
  LOG_ERR << "[dumpCallback]" << GetUTCTimeStr() << "dmp path: " << d.path();
  void* frames[128];
  logGetBackTrace(frames, 128);
  return succeeded;
}

// Static initialisers in monitor_main.cpp (0x487978): installed before main() runs.
google_breakpad::MinidumpDescriptor descriptor(vmpath::CRASH_DUMP_DIR);
google_breakpad::ExceptionHandler eh(descriptor, nullptr, dumpCallback, nullptr, true, -1);
#endif
} // namespace

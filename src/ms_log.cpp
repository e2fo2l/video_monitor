#include "ms_log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

#include <rapidjson/document.h>

#include "util/thread_name.h"
#include "vm_paths.h"

namespace
{
constexpr int kMaxLogSize = 0x100000;
} // namespace

// 0x45be00
MSLog& MSLog::getInstance()
{
  // Intentionally leaked, like the original, so logging still works from destructors and
  // threads during exit.
  static auto* instance = new MSLog(); // NOLINT(cppcoreguidelines-owning-memory)
  return *instance;
}

// 0x45bbac
MSLog::MSLog()
    : m_path(vmpath::LOG_FILE)
{
  // Keep appending to whichever of the two files is smaller.
  if (access(vmpath::LOG_FILE, F_OK) == 0 && access(vmpath::LOG_FILE_BAK, F_OK) == 0)
  {
    if (GetFileSize(vmpath::LOG_FILE_BAK) < GetFileSize(vmpath::LOG_FILE))
      m_path = vmpath::LOG_FILE_BAK;
  }
  m_file = std::make_unique<std::ofstream>(m_path, std::ios::app);
  GetConfigFromFile(vmpath::LOG_SWITCH_CFG);
  std::thread(&MSLog::ThreadSwitchLogFile, this).detach();
}

// 0x45bf28 — {"cam_mon": <int>} != 0 enables verbose logging.
void MSLog::GetConfigFromFile(const std::string& path)
{
  int level = MSLOG_ERROR;
  if (access(path.c_str(), F_OK) == 0)
  {
    std::ifstream in(path);
    if (in.is_open())
    {
      std::string const text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
      rapidjson::Document doc;
      if (!doc.Parse(text.c_str()).HasParseError() && doc.IsObject())
      {
        const auto it = doc.FindMember("cam_mon");
        if (it != doc.MemberEnd() && it->value.IsInt() && it->value.GetInt() != 0)
          level = MSLOG_VERBOSE;
      }
    }
  }
  m_level = level;
}

// 0x45c160
std::string MSLog::transLevel2String(MSLogLevel level, const char* fileLine)
{
  std::string s;
  switch (level)
  {
  case MSLOG_WARNING:
    s += "[WARNING ";
    break;
  case MSLOG_INFO:
    s += "[INFO ";
    break;
  case MSLOG_DEBUG:
    s += "[DEBUG ";
    break;
  case MSLOG_VERBOSE:
    s += "[VERBOSE ";
    break;
  default:
    s += "[ERROR ";
    break;
  }
  s += GetFormatNowTime();
  s += " ";
  if (const char* slash = std::strrchr(fileLine, '/'))
    fileLine = slash + 1;
  s += fileLine;
  return s;
}

// 0x45c508 — "YYYY-mm-dd HH:MM:SS.uuuuuu"
std::string MSLog::GetFormatNowTime()
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[80];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  std::string s = buf;
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
  std::string frac = std::to_string(us % 1000000);
  frac.insert(0, 6 - std::min<size_t>(frac.size(), 6), '0');
  s += '.';
  s += frac;
  return s;
}

// 0x45c40c
int MSLog::GetFileSize(const char* path)
{
  struct stat st{};
  if (stat(path, &st) != 0)
    return -1;
  return static_cast<int>(st.st_size);
}

// operator<<(std::endl) at 0x45c2dc
void MSLog::write(const std::string& line)
{
  std::scoped_lock const lock(m_mutex);
  *m_file << line << '\n';
  m_file->flush();
}

// 0x45c6ec — ping-pong between .log and .log.bak once the active file passes 1 MiB.
void MSLog::ThreadSwitchLogFile()
{
  SetThreadName("ThreadSwitchMSLogFile");
  for (;;)
  {
    if (GetFileSize(m_path.c_str()) >= kMaxLogSize)
    {
      m_path = (m_path == vmpath::LOG_FILE) ? vmpath::LOG_FILE_BAK : vmpath::LOG_FILE;
      std::scoped_lock const lock(m_mutex);
      m_file = std::make_unique<std::ofstream>(m_path, std::ios::trunc);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

// log_zm.cpp — 0x45b168 / 0x45b254 / 0x45b340 / 0x45b42c
void log_err(const char* msg) { LOG_ERR << msg; }
void log_info(const char* msg) { LOG_INFO << msg; }
void log_warn(const char* msg) { LOG_WARN << msg; }
void log_for_ali(int level, const char* msg)
{
  if (MSLog::getInstance().getLogLevel() >= level)
    MSLogLine(static_cast<MSLogLevel>(level), __FILE__ ":" VM_STR(__LINE__)) << msg;
}

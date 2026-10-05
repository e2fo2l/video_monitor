// MSLog — the process-wide file logger (/data/log/video_monitor.log, rotated at 1 MiB).
#pragma once

#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

enum MSLogLevel : int
{
  MSLOG_ERROR = 2,
  MSLOG_WARNING = 3,
  MSLOG_INFO = 4,
  MSLOG_DEBUG = 5,
  MSLOG_VERBOSE = 6,
};

class MSLog
{
public:
  static MSLog& getInstance();

  int getLogLevel() const { return m_level; }
  void GetConfigFromFile(const std::string& path);

  // "[ERROR 2024-01-01 12:00:00.123456 monitor_main.cpp:81"
  static std::string transLevel2String(MSLogLevel level, const char* fileLine);
  static std::string GetFormatNowTime();
  static int GetFileSize(const char* path);

  void write(const std::string& line);

private:
  MSLog();
  [[noreturn]] void ThreadSwitchLogFile();

  std::unique_ptr<std::ofstream> m_file;
  std::string m_path;
  int m_level = MSLOG_ERROR;
  std::mutex m_mutex;
};

// One log statement. The original accumulated into a single shared string inside MSLog
// (not thread safe); here every statement owns its buffer and is flushed atomically.
class MSLogLine
{
public:
  MSLogLine(MSLogLevel level, const char* fileLine) { m_ss << MSLog::transLevel2String(level, fileLine) << "] "; }
  MSLogLine(const MSLogLine&) = delete;
  ~MSLogLine() { MSLog::getInstance().write(m_ss.str()); }

  template <typename T> MSLogLine& operator<<(const T& v)
  {
    m_ss << v;
    return *this;
  }

private:
  std::ostringstream m_ss;
};

#define VM_STR2(x) #x
#define VM_STR(x) VM_STR2(x)
#define VM_LOG(level)                                                                                                  \
  for (bool vm_log_on_ = MSLog::getInstance().getLogLevel() >= (level); vm_log_on_; vm_log_on_ = false)                \
  MSLogLine((level), __FILE__ ":" VM_STR(__LINE__))

#define LOG_ERR VM_LOG(MSLOG_ERROR)
#define LOG_WARN VM_LOG(MSLOG_WARNING)
#define LOG_INFO VM_LOG(MSLOG_INFO)
#define LOG_DBG VM_LOG(MSLOG_DEBUG)

// C-style hooks exported for the vendor SDK log callbacks (log_zm.cpp).
extern "C"
{
  void log_err(const char* msg);
  void log_info(const char* msg);
  void log_warn(const char* msg);
  void log_for_ali(int level, const char* msg);
}

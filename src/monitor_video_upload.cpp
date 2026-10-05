// monitor_video_upload.cpp + monitor_video_record.cpp: curl upload and thumbnail helpers.
// Only UploadFileByCurl is reachable in the shipped binary; the rest is linked-in dead code.
#include "monitor_common.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ios>
#include <ostream>
#include <string>

#include <sys/stat.h>

#include <curl/curl.h>

extern "C"
{
#include <libavformat/avformat.h>
}

#include "monitor_video.h"
#include "ms_log.h"
#include "util/thread_name.h"
#include "vm_paths.h"

namespace
{
int g_uploadProgress = 0; // DAT_0053fd8c
}

// 0x4a3738 — CURLOPT_PROGRESSFUNCTION.
// NOTE(orig): returns the percentage, and any non-zero return aborts the transfer in libcurl.
int MonitorVideo::ProcessUpdate(void* /*unused*/, double /*unused*/, double /*unused*/, double ultotal, double ulnow)
{
  if (CheckZero(ultotal))
  {
    g_uploadProgress = 100;
    return 100;
  }
  const int pct = static_cast<int>(ulnow * 100.0 / ultotal);
  g_uploadProgress = std::max(pct, g_uploadProgress);
  return pct;
}

// 0x4a37e0 — dead code.
void MonitorVideo::VideoUpload(int taskid, long ossId, const std::string& url, const std::string& name)
{
  SetThreadName("monitor_video_upload");
  g_uploadProgress = 0;
  const std::string operType = (taskid == TASK_MP4) ? "download" : "record";
  const int replyTask = (taskid == TASK_MP4) ? TASK_MP4 : TASK_THUMB;
  const std::string path = GetSysConf().video_filepath + name;

  // NOLINTBEGIN(cppcoreguidelines-owning-memory): libcurl's default read callback needs a C stdio
  // handle (CURLOPT_READDATA); it is closed on every path below.
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp)
  {
    LOG_ERR << "[upload]file open failed:" << path;
    setMMIState(MMI_SYS_IDLE);
    ResponseToApp("oss_upload_url", replyTask, operType, "end", VM_ERR_FILE_OPEN, 0, ossId);
    return;
  }
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
  {
    LOG_ERR << "[upload]file get stat failed:" << path;
    setMMIState(MMI_SYS_IDLE);
    ResponseToApp("oss_upload_url", replyTask, operType, "end", VM_ERR_FILE_STAT, 0, ossId);
    fclose(fp);
    return;
  }
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    LOG_ERR << "[upload]curl init failed!";
    setMMIState(MMI_SYS_IDLE);
    ResponseToApp("oss_upload_url", replyTask, operType, "end", VM_ERR_CURL, 0, ossId);
    fclose(fp);
    return;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg): libcurl's API is variadic
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_CAPATH, vmpath::CA_CERT_DIR);
  curl_easy_setopt(curl, CURLOPT_READDATA, fp);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(st.st_size));
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM < 0x072000
  curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &MonitorVideo::ProcessUpdate);
#else
  curl_easy_setopt(
      curl, CURLOPT_XFERINFOFUNCTION, +[](void* p, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) -> int
      { return ProcessUpdate(p, double(dt), double(dn), double(ut), double(un)); });
#endif
  // NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  const CURLcode rc = curl_easy_perform(curl);
  int ret = VM_OK;
  if (rc == CURLE_OK)
  {
    curl_off_t speed = 0;
    curl_off_t total = 0;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg): libcurl's API is variadic
    curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD_T, &speed);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total);
    // NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  }
  else
  {
    LOG_ERR << "[upload]curl perform failed!";
    ret = VM_ERR_CURL;
    curl_easy_strerror(rc);
  }
  curl_easy_cleanup(curl);
  fclose(fp);
  // NOLINTEND(cppcoreguidelines-owning-memory)
  if (rc == CURLE_OK && name.find(".thumb") != std::string::npos)
    std::remove(path.c_str());
  setMMIState(MMI_SYS_IDLE);
  ResponseToApp("oss_upload_url", replyTask, operType, "end", ret, 0, ossId);
}

// 0x4a4138 — HTTP PUT with up to 3 attempts.
bool MonitorVideo::UploadFileByCurl(const std::string& url, const std::string& path, const std::string& caPath,
                                    int& fileSize)
{
  // NOLINTBEGIN(cppcoreguidelines-owning-memory): libcurl's default read callback needs a C stdio
  // handle (CURLOPT_READDATA); it is closed on every path below.
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp)
    return false;
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
  {
    fclose(fp);
    return false;
  }
  fileSize = static_cast<int>(st.st_size);
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    fclose(fp);
    return false;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg): libcurl's API is variadic
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CAPATH, caPath.c_str());
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_READDATA, fp);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(st.st_size));
  // NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  bool ok = false;
  for (int i = 0; i < 3; ++i)
  {
    // NOTE(orig): the file is not rewound between attempts.
    if (curl_easy_perform(curl) == CURLE_OK)
    {
      ok = true;
      break;
    }
  }
  curl_easy_cleanup(curl);
  fclose(fp);
  // NOLINTEND(cppcoreguidelines-owning-memory)
  return ok;
}

// 0x4a3260 — milliseconds; dead code.
unsigned MonitorVideo::GetVideoDuration(const std::string& name)
{
  const std::string path = GetSysConf().video_filepath + name;
  avformat_network_init();
  AVFormatContext* ctx = avformat_alloc_context();
  if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
  {
    LOG_ERR << "[GetVideoDuration]open failed:" << name;
    return 0xffffffffU;
  }
  const auto ms = static_cast<unsigned>(ctx->duration / 1000);
  avformat_close_input(&ctx); // NOTE(orig): leaked
  return ms;
}

// 0x4a3194 — dead code.
int monitor_video_get_tduration(const char* name)
{
  const std::string path = GetSysConf().video_filepath + name;
  avformat_network_init();
  AVFormatContext* ctx = avformat_alloc_context();
  // NOTE(orig): the open result is ignored; a failed open dereferences NULL.
  if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
    return -1;
  const int ms = static_cast<int>(ctx->duration / 1000);
  avformat_close_input(&ctx);
  return ms;
}

// 0x4a33f8 — "<dir><timestamp>" text+binary thumbnail container; dead code.
bool MonitorVideo::SaveVideoThumb(const RecordTitleType& t)
{
  std::ofstream out;
  out.open(GetSysConf().video_filepath + t.timestamp, std::ios::out);
  out << "timestamp:" << t.timestamp << '\n';
  out << "filename:" << t.filename << '\n';
  out << "filesize:" << t.filesize << '\n';
  out << "video_duration:" << t.duration << '\n';
  out << "thumbnail_data:";
  out.write(t.thumb, t.thumbSize);
  out << '\n';
  out << "thumbnail_size:" << t.thumbSize << '\n';
  out.close();
  return true;
}

// 0x4a2ea0 — C variant of the above; dead code.
int monitor_video_save_thumbfile(s_record_title title)
{
  std::ofstream out(GetSysConf().video_filepath + title.timestamp + ".thumb", std::ios::binary);
  if (!out)
    return -1;
  out << "timestamp:" << title.timestamp << '\n';
  out << "filename:" << title.filename << '\n';
  out << "filesize:" << static_cast<long>(title.filesize) << '\n';
  out << "video_duration:" << static_cast<int>(title.duration) << '\n';
  out << "thumbnail_data:";
  out.write(title.thumb, title.thumbSize);
  out << '\n';
  out << "thumbnail_size:" << static_cast<long>(title.thumbSize) << '\n';
  return 0;
}

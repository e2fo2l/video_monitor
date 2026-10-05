// MonitorVideo — app-facing camera operations (live view, recording, playback, file
// management, cloud upload) plus the TRecorder glue in monitor_record.cpp.
#pragma once

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <vendor/trecorder.h>

#include "monitor_common.h"

struct AVFormatContext;
struct AVStream;
struct AVPacket;

struct UploadFileMess
{
  std::string url;      // +0x00
  std::string filename; // +0x20
  long ossId = 0;       // +0x40
  std::string key;      // +0x48
};

// C layout used by the (unused) thumbnail writer, 0x198 bytes.
struct s_record_title
{
  char timestamp[0x80];
  char filename[0x100];
  unsigned filesize;
  unsigned duration;
  char* thumb;
  int thumbSize;
};

struct RecordTitleType
{
  std::string timestamp;
  std::string filename;
  std::string reserved;
  int filesize = 0;
  int duration = 0;
  const char* thumb = nullptr;
  int thumbSize = 0;
};

// One TRecorder channel, 0x90 bytes.
struct RecoderTestContext
{
  TrecoderHandle recorder; // +0x00
  int index;               // +0x08
  int segmentCounter;      // +0x0c
  char outputPath[0x80];   // +0x10
};

struct RecorderStatusContext
{ // 0x14 bytes
  int state;
  int reserved;
  int a;
  int b;
  int started;
};

class MonitorVideo
{
public:
  static MonitorVideo& GetPtr();

  // operType handlers
  void MonitorVideoControl(MonitorJson& value, MonitorJson& msg);
  void PlayBackVideoControl(MonitorJson& value, MonitorJson& msg);
  void RecordVideoControl(MonitorJson& value, MonitorJson& msg);
  void DeleteVideoControl(MonitorJson& value, MonitorJson& msg);
  void RenameVideoControl(MonitorJson& value, MonitorJson& msg);
  void DownloadVideoControl(MonitorJson& value, MonitorJson& msg);
  void UploadVideoControl(MonitorJson& value, MonitorJson& msg);
  void ResetVideoControl(MonitorJson& value, MonitorJson& msg);
  void GetPropVideoControl(MonitorJson& value, MonitorJson& msg);
  void CheckDiskVideoControl(MonitorJson& value, MonitorJson& msg);
  void KeepAliveVideoControl(MonitorJson& value, MonitorJson& msg);
  void RestAliService(MonitorJson&, MonitorJson&) {}

  uint16_t CloseMonitor(const std::string& tag);

  // playback (monitor_video_playback.cpp)
  void VideoPlaybackStart(const std::string& name);
  void VideoPlaybackEndClean(AVFormatContext* ctx, bool disconnect, const std::string& path, bool ok);
  void VideoPlaybackStop();

  // upload / thumbnails (monitor_video_upload.cpp, mostly dead code)
  static int ProcessUpdate(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow);
  void VideoUpload(int taskid, long ossId, const std::string& url, const std::string& name);
  bool UploadFileByCurl(const std::string& url, const std::string& path, const std::string& caPath, int& fileSize);
  unsigned GetVideoDuration(const std::string& name);
  bool SaveVideoThumb(const RecordTitleType& t);

private:
  MonitorVideo();

  void SolveUploadUrlProblem(int expires, const std::string& filename);
  bool getOneUploadFileMess(UploadFileMess& out);
  void setOneUploadFileMess(const UploadFileMess& m);
  [[noreturn]] void UploadFile2Sever();
  void SolveUploadFileProblem(bool ok, long ossId, const std::string& filename, const std::string& key, int fileSize);

  std::string m_token;                      // +0x00
  std::string m_channelId;                  // +0x20
  std::string m_encryptionKey;              // +0x40
  std::string m_session;                    // +0x60
  std::atomic<bool> m_bPlayback{false};     // +0x80
  std::queue<UploadFileMess> m_uploadQueue; // +0x88
  std::mutex m_uploadMutex;                 // +0xd8
};

// monitor_record.cpp
extern RecoderTestContext recorderTestContext[2];
extern RecorderStatusContext RecorderStatus[2];
int CallbackFromTRecorder(void* user, int msg, void* payload);
void ResetSpsPps();
const std::vector<uint8_t>& GetSpsPps(); // Annex-B SPS+PPS for the configured encoder size
int oneChannelTest(RecoderTestContext* ctx, RecorderStatusContext* status, int index);
// Exported (with -rdynamic): LD_PRELOAD shims like vacuumstreamer call these via dlsym().
[[gnu::visibility("default")]] uint16_t startMonitor();
[[gnu::visibility("default")]] int endMonitor();
int startRecord(const std::string& name);
int endRecord();
[[noreturn]] void check_disk_task(int);
void SolveRecordFile(const std::string& name, int durationSec);

// monitor_video_playback.cpp
int GetVideoFrameEnd(AVFormatContext* ctx);
int en_queue(AVStream* st, AVPacket* pkt);

// monitor_video_upload.cpp
int monitor_video_save_thumbfile(s_record_title title);
int monitor_video_get_tduration(const char* name);

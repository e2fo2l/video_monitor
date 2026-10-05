// TRecorder glue: camera capture -> H.264 -> MonitorAgoraService queue, plus local recording.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "monitor_agora_service.h"
#include "monitor_common.h"
#include "monitor_json.h"
#include "monitor_msg_center.h"
#include "monitor_video.h"
#include "ms_log.h"
#include "util/h264_param_sets.h"
#include "util/thread_name.h"
#include "vendor/trecorder.h"

RecoderTestContext recorderTestContext[2];
RecorderStatusContext RecorderStatus[2];

namespace
{
[[maybe_unused]] int devVideoIndex = 0;
s_record_title up2server_title;
int muxer_enable_value = 0;
[[maybe_unused]] int stop_record_F = 0;

// SPS+PPS (Annex-B) spliced in front of every IDR slice that arrives without in-band
// parameter sets.
//
// The original carried five 22-byte tables (spsAndPps180_/360_/480_/504_/720_). They differ
// only in frame size, so the blob is generated from the configured width/height; for those
// five sizes the output is byte-identical to the tables. With --compat=y, InitVideoResolution
// only accepts those five sizes (anything else stays 864x480), like the original. Either way it
// assumes the encoder's other settings (Main@3.1, CABAC, POC type 0, 8-bit frame_num/POC lsb,
// 1 ref frame); by default the encoder's own in-band SPS/PPS are preferred when it sends them.
// A function-local static, so a failing allocation cannot throw before main().
std::vector<uint8_t>& SpsPps()
{
  static std::vector<uint8_t> spsPps = BuildSpsPps(864, 480);
  return spsPps;
}
bool g_insertSpsPps = true; // DAT_0052f0d0
bool g_recordDone = true;   // DAT_0052f0c6
int g_muxIdrCount = 0;      // DAT_0053fd20
std::string g_recordName;   // DAT_0053fd18, timestamp-ish name of the clip being muxed

const char kCameraNames[2][6] = {"front", "rear"};

bool isAnnexBIdr(const uint8_t* p)
{
  return (p != nullptr) && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1 && (p[4] & 0x1f) == 5;
}

#if !VM_COMPAT
// Learn the encoder's real parameter sets whenever it emits them in-band.
std::vector<uint8_t> g_learnedSpsPps;

void learnSpsPps(const uint8_t* p, int size)
{
  if (!p || size < 5 || p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 1 || (p[4] & 0x1f) != 7)
    return;
  // Copy everything up to the first slice NAL (type 1 or 5).
  for (int i = 4; i + 4 < size; ++i)
  {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
    {
      const int nal = p[i + 3] & 0x1f;
      if (nal == 1 || nal == 5)
      {
        const int end = (i > 0 && p[i - 1] == 0) ? i - 1 : i;
        g_learnedSpsPps.assign(p, p + end);
        return;
      }
    }
  }
}
#endif

// T_RECORD_MUX_STATUS == 1 (inlined in CallbackFromTRecorder in the original): the clip being
// muxed is complete.
void OnRecordMuxed()
{
  const std::string endTime = getCurrentSystemTimeChrono();
  const double diff = std::difftime(String2Time_t(endTime), String2Time_t(g_recordName));
  const int seconds = static_cast<int>(std::abs(diff));
  LOG_INFO << "[CallbackFromTRecorder]strEndTime = " << endTime;
  LOG_INFO << "[CallbackFromTRecorder]ct = " << g_recordName;
  SolveRecordFile(g_recordName, seconds);
  g_recordDone = true;
  g_muxIdrCount = 0;
  g_recordName.clear();
  const MONITOR_SYS_STATE s = getMonitorState();
  if (s == MONITOR_RECORD_START)
    setMonitorState(MONITOR_VIDEO_START);
  else if (s == MONITOR_TALK_RECORD_START)
    setMonitorState(MONITOR_TALK_START);
}
} // namespace

// 0x493ad8
int CallbackFromTRecorder(void* user, int msg, void* payload)
{
  auto* ctx = static_cast<RecoderTestContext*>(user);
  if (!ctx)
  {
    LOG_ERR << "[CallbackFromTRecorder]trTestContext is null";
    return -1;
  }
  const MONITOR_SYS_STATE state = getMonitorState();
  muxer_enable_value = (state == MONITOR_RECORD_START || state == MONITOR_TALK_RECORD_START) ? 1 : 0;

  switch (msg)
  {
  case T_RECORD_STREAM_FRAME:
  {
    auto* frame = static_cast<TRecorderVideoFrame*>(payload);
    if (!frame || !frame->data)
    {
      LOG_ERR << "[CallbackFromTRecorder]buff or buf is null";
      return -1;
    }
    if (muxer_enable_value == 0)
      g_muxIdrCount = 0;

    const uint8_t* src = frame->data;
    bool const idr = isAnnexBIdr(src);
    if (idr && (muxer_enable_value != 0))
      ++g_muxIdrCount;

    H264Buffer buf;
#if !VM_COMPAT
    learnSpsPps(src, frame->size);
    const std::vector<uint8_t>& params = g_learnedSpsPps.empty() ? SpsPps() : g_learnedSpsPps;
#else
    const std::vector<uint8_t>& params = SpsPps();
#endif
    const uint8_t* hdr = params.data();
    const size_t hdrLen = params.size();
    buf.size = (idr && g_insertSpsPps) ? frame->size + static_cast<int>(hdrLen) : frame->size;
    buf.isKeyFrame = idr;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): queued raw buffer, freed by SendVideoToAgoraThread
    buf.data = static_cast<uint8_t*>(malloc(static_cast<size_t>(buf.size)));

    if (idr && g_insertSpsPps)
    {
      memcpy(buf.data, hdr, hdrLen);
      memcpy(buf.data + hdrLen, src, static_cast<size_t>(buf.size) - hdrLen);
    }
    else
    {
      memcpy(buf.data, src, static_cast<size_t>(buf.size));
      if (frame->is_key_frame != 0)
      {
        // Encoder already put SPS/PPS in band (frame starts with NAL 7).
        buf.isKeyFrame = true;
        if (muxer_enable_value != 0)
          ++g_muxIdrCount;
      }
    }
    MonitorAgoraService::GetPtr().setQueueH264Buffer(buf);
    break;
  }

  case T_RECORD_MUX_STATUS:
    if (*static_cast<int*>(payload) == 1)
      OnRecordMuxed();
    break;

  case T_RECORD_ONE_FILE_COMPLETE:
    if (++ctx->segmentCounter > 40)
      ctx->segmentCounter = 0;
    {
      memset(ctx->outputPath, 0, sizeof ctx->outputPath);
      const std::string path = GetSysConf().video_filepath + g_recordName + ".mp4";
      path.copy(ctx->outputPath, sizeof ctx->outputPath - 1); // truncated like the original's snprintf
      memset(&up2server_title, 0, sizeof up2server_title);
      if (ctx->recorder)
        TRchangeOutputPath(ctx->recorder, ctx->outputPath);
      break;
    }

  default:
    LOG_ERR << "[CallbackFromTRecorder]warning: unknown callback from trecorder";
    break;
  }
  return 0;
}

// 0x494430 — originally a switch over the five tables.
void ResetSpsPps() { SpsPps() = BuildSpsPps(GetVideoWidth(), GetVideoHeight()); }

const std::vector<uint8_t>& GetSpsPps() { return SpsPps(); }

namespace
{
// 0x494500
[[maybe_unused]] int outputRkLog(const char* msg)
{
  MSLog::getInstance().write(msg);
  return 0;
}
} // namespace

// 0x494548 — create, configure and start one TRecorder channel. Exits the process on failure.
int oneChannelTest(RecoderTestContext* ctxs, RecorderStatusContext* status, int index)
{
  RecoderTestContext& ctx = ctxs[index];
  memset(&ctx, 0, sizeof ctx);
  ctx.recorder = CreateTRecorder();
  if (!ctx.recorder)
  {
    LOG_ERR << "[CallbackFromTRecorder]CreateTRecorder[" << index << "]err";
    return -1;
  }
  ctx.index = index;
  TRreset(ctx.recorder);
  const std::string cfg = GetRecorderCfgPath();
  TRSetRecorderCfgPath(cfg.c_str(), static_cast<int>(cfg.length()));
  ResetSpsPps();
  TRsetCamera(ctx.recorder, index);
  status[index].b = 0;
  status[index].a = 0;

  const std::string& dir = GetSysConf().video_filepath;
  if (access(dir.c_str(), F_OK) != 0 && mkdir(dir.c_str(), 0777) == -1)
    LOG_ERR << "mkdir " << dir << " fail";

  // TRsetOutput takes a writable buffer; truncated to 63 characters like the original's.
  std::array<char, 0x40> out{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): camera index, 0 or 1 (always 0 here)
  (dir + "AW_" + kCameraNames[index] + "_video0.mp4").copy(out.data(), out.size() - 1);
  TRsetOutput(ctx.recorder, out.data());
  TRsetMaxRecordTimeMs(ctx.recorder, 60000);
  TRsetRecorderCallback(ctx.recorder, CallbackFromTRecorder, &ctx);
  if (TRprepare(ctx.recorder) < 0)
  {
    LOG_ERR << "[CallbackFromTRecorder]trecorder " << index << " prepare error";
    std::exit(1); // NOLINT(concurrency-mt-unsafe): the original exits the whole process here
  }
  if (TRstart(ctx.recorder, 2) < 0)
  {
    LOG_ERR << "[CallbackFromTRecorder]trecorder " << index << " start error";
    std::exit(1); // NOLINT(concurrency-mt-unsafe): the original exits the whole process here
  }
  status[index].state = 0;
  status[index].started = 1;
  return 0;
}

// 0x494d9c
uint16_t startMonitor()
{
  const int ret = oneChannelTest(recorderTestContext, RecorderStatus, 0);
  if (ret == 0)
    return VM_OK;
  LOG_ERR << "recordControl fail: " << ret;
  return VM_ERR_RECORDER_START;
}

// 0x494ed0
int endMonitor()
{
  TRstop(recorderTestContext[0].recorder, 2);
  TRrelease(recorderTestContext[0].recorder);
  return 0;
}

// 0x494f40
int startRecord(const std::string& name)
{
  g_recordName = name;
  const int ret = TRmux(recorderTestContext[0].recorder, 3);
  if (ret != 0)
    LOG_ERR << "[record]start record fail!";
  return ret;
}

// 0x49506c
int endRecord()
{
  const int ret = TRmux(recorderTestContext[0].recorder, 4);
  LOG_INFO << "[record] end record TRmux ret: " << ret;
  return ret;
}

// 0x495188 — never started by the shipped binary.
void check_disk_task(int /*unused*/)
{
  SetThreadName("VMS_check_disk");
  MonitorJson value;
  MonitorJson msg;
  value.AddString("guid", "666");
  value.AddString("operType", "checkdisk");
  value.AddString("operation", "start");
  value.AddInt("total", GetSysConf().video_total_disk_space);
  value.AddInt("result", 666);
  value.AddInt("status", 666);
  value.AddInt("used", 0);
  msg.AddString("method", "get_disksize");
  msg.AddInt("taskid", TASK_DISK_PROP);

  auto report = [&](long used)
  {
    value.RemoveMember("used");
    value.AddInt("used", used);
    msg.AddString("vulue", value.ToString()); // sic
    MonitorMsgCenter::GetPtr().SendMsg(msg.ToString());
  };

  for (;;)
  {
    while (getMonitorState() != MONITOR_RECORD_START)
      std::this_thread::sleep_for(std::chrono::seconds(1));
    const int freeMb = static_cast<int>(GetDiskFreeSize(""));
    const SysCfgParamType& c = GetSysConf();
    if (freeMb > c.video_file_size)
    {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    // Two separate branches with the same body in the original.
    if (freeMb > c.video_least_file_size || freeMb > c.video_least_disk_space)
    {
      report(c.video_total_disk_space - freeMb);
    }
    else
    {
      report(c.video_total_disk_space);
      endRecord();
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
  }
}

// 0x495b5c — rename "<name>.mp4/.jpg" to "<dur>_<name>.mp4/.jpg" and ask the app for an
// upload URL.
void SolveRecordFile(const std::string& name, int durationSec)
{
  if (name.empty())
  {
    LOG_ERR << "[SolveRecordFile] strFileName is empty!";
    return;
  }
  const std::string& dir = GetSysConf().video_filepath;
  const std::string mp4 = dir + name + ".mp4";
  const std::string jpg = dir + name + ".jpg";
  if (!CheckFileExist(mp4))
  {
    std::remove(jpg.c_str());
    LOG_ERR << "[SolveRecordFile]" << mp4 << "not exist!";
    return;
  }
  if (!CheckFileExist(jpg))
  {
    std::remove(mp4.c_str());
    LOG_ERR << "[SolveRecordFile]" << jpg << "not exist!";
    return;
  }
  std::string key;
  if (!getRoundData(16, key))
  {
    std::remove(mp4.c_str());
    std::remove(jpg.c_str());
    LOG_ERR << "[SolveRecordFile] getRoundData error!";
    return;
  }
  const std::string newMp4 = dir + std::to_string(durationSec) + "_" + name + ".mp4";
  const std::string newJpg = dir + std::to_string(durationSec) + "_" + name + ".jpg";
  rename(mp4.c_str(), newMp4.c_str());
  rename(jpg.c_str(), newJpg.c_str());
  MonitorMsgCenter::GetPtr().SendMsg(JsonForUploadUrl("mp4", newMp4, key, 0, TASK_MP4).ToString());
}

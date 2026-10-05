#include "monitor_video.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include "monitor_agora_service.h"
#include "monitor_audio.h"
#include "monitor_common.h"
#include "monitor_json.h"
#include "monitor_msg_center.h"
#include "ms_log.h"
#include "util/thread_name.h"
#include "vm_paths.h"

// Static-init singleton (0x49da0c).
MonitorVideo& MonitorVideo::GetPtr()
{
  // Never destroyed, like the original, so no destructor runs at exit while the worker
  // threads still use it.
  static auto* instance = new MonitorVideo(); // NOLINT(cppcoreguidelines-owning-memory)
  return *instance;
}

// 0x497b70
MonitorVideo::MonitorVideo() { std::thread(&MonitorVideo::UploadFile2Sever, this).detach(); }

// 0x497d74 — operType "monitor": start/end live view.
void MonitorVideo::MonitorVideoControl(MonitorJson& value, MonitorJson& msg)
{
  const std::string tag = "[Monitor]";
  std::string operation;
  if (!GetMsgParam(value, msg, "operation", tag, operation, false) ||
      !GetMsgParam(value, msg, "session", tag, m_session, false))
    return;
  if (m_session.empty())
  {
    LOG_ERR << "[Monitor]empty parameter session" << m_session;
    ResponseToApp(value, msg, VM_ERR_MISSING_PARAM);
    return;
  }

  const MONITOR_SYS_STATE state = getMonitorState();
  if (operation == "end")
  {
    LOG_ERR << "[Monitor]agora, map clear";
    ClearSession();
    ResponseToApp(value, msg, CloseMonitor("[monitor]"));
    return;
  }
  if (operation != "start")
  {
    LOG_ERR << "[Monitor]invalid operation:" << operation;
    ResponseToApp(value, msg, VM_ERR_INVALID_OPERATION);
    return;
  }
  if (state != MONITOR_SYS_BUTT)
  {
    LOG_ERR << "[Monitor]The state is " << MonitorStateToStr(state);
    ResponseToApp(value, msg, VM_ERR_BUSY);
    return;
  }
  if (!GetMsgParam(value, msg, "token", tag, m_token, false) ||
      !GetMsgParam(value, msg, "channelId", tag, m_channelId, false) ||
      !GetMsgParam(value, msg, "encryptionKey", tag, m_encryptionKey, false))
    return;
  if (m_token.empty() || m_channelId.empty() || m_encryptionKey.empty())
  {
    LOG_ERR << "[Monitor]empty parameter token:" << m_token << " channelId:" << m_channelId;
    ResponseToApp(value, msg, VM_ERR_MISSING_PARAM);
    return;
  }

  LOG_INFO << "[Monitor]monitor start token:" << m_token << " channelId:" << m_channelId << " session" << m_session;
  MonitorAgoraService& agora = MonitorAgoraService::GetPtr();
  if (!agora.MonitorAgoraConnect(m_token, m_channelId, m_encryptionKey))
  {
    agora.MonitorAgoraDisConnect();
    LOG_ERR << "[Monitor]connect agora failed!";
    ResponseToApp(value, msg, VM_ERR_AGORA_CONNECT);
    return;
  }
  const uint16_t ret = startMonitor();
  if (ret == VM_OK)
  {
    setMonitorState(MONITOR_VIDEO_START);
    LOG_INFO << "[Monitor]start monitor!";
    agora.SetStreamCount(0);
  }
  else
  {
    agora.MonitorAgoraDisConnect();
    LOG_ERR << "[Monitor]start monitor failed!";
  }
  ResponseToApp(value, msg, ret);
}

// 0x498f4c — operType "playback".
void MonitorVideo::PlayBackVideoControl(MonitorJson& value, MonitorJson& msg)
{
  std::string operation;
  std::string videoName;
  if (!GetMsgParam(value, msg, "operation", "[PlayBack]", operation, false) ||
      !GetMsgParam(value, msg, "token", "[PlayBack]", m_token, false) ||
      !GetMsgParam(value, msg, "channelId", "[PlayBack]", m_channelId, false) ||
      !GetMsgParam(value, msg, "videoName", "[PlayBack]", videoName, false) ||
      !GetMsgParam(value, msg, "encryptionKey", "[Monitor]", m_encryptionKey, false))
    return;

  const MONITOR_SYS_STATE state = getMonitorState();
  if (operation == "start")
  {
    if (state != MONITOR_SYS_BUTT)
    {
      LOG_ERR << "[playBack] start invalid state:" << MonitorStateToStr(state);
      ResponseToApp(value, msg, VM_ERR_BUSY);
      return;
    }
    if (!CheckFileExist(videoName))
    {
      LOG_ERR << "[playback]video file does not exists:" << videoName;
      ResponseToApp(value, msg, VM_ERR_FILE_NOT_EXIST);
      return;
    }
    setMonitorState(PLAYBACK_VIDEO_START);
    m_bPlayback = true;
    // NOTE(orig): std::thread(std::bind(&VideoPlaybackStart, this, _1), token, channelId,
    // encryptionKey, videoName) — bind forwards only the first extra argument, so the
    // worker receives the *token* as the file name.
    std::thread(&MonitorVideo::VideoPlaybackStart, this, m_token).detach();
  }
  else if (operation == "end")
  {
    if (state != PLAYBACK_VIDEO_START)
    {
      LOG_ERR << "[playback]end invalid stats:" << MonitorStateToStr(state);
      ResponseToApp(value, msg, VM_ERR_PLAYBACK_BAD_STATE);
      return;
    }
    VideoPlaybackStop();
  }
  else
  {
    LOG_ERR << "[playback]invalid operation:" << operation;
    ResponseToApp(value, msg, VM_ERR_INVALID_OPERATION);
    return;
  }
  ResponseToApp(value, msg, VM_OK);
}

// 0x499b5c — operType "recordVideo": mux the live stream to an mp4 named after "thumb".
void MonitorVideo::RecordVideoControl(MonitorJson& value, MonitorJson& msg)
{
  std::string operation;
  std::string name;
  if (!GetMsgParam(value, msg, "operation", "[Record]", operation, false))
    return;
  int ret = VM_OK;
  MONITOR_SYS_STATE state = getMonitorState();
  if (operation == "start")
  {
    if (!GetMsgParam(value, msg, "thumb", "[Record]", name, false))
      return;
    if (state == MONITOR_VIDEO_START || state == MONITOR_TALK_START)
    {
      ret = startRecord(name);
      if (ret == 0)
        setMonitorState(state == MONITOR_VIDEO_START ? MONITOR_RECORD_START : MONITOR_TALK_RECORD_START);
      else
        ret = VM_ERR_RECORD_START;
    }
    else
    {
      LOG_ERR << "[record]start invalid stats:" << MonitorStateToStr(state);
      ret = VM_ERR_RECORD_BAD_STATE;
    }
  }
  else if (operation == "end")
  {
    state = getMonitorState();
    if (state == MONITOR_RECORD_START || state == MONITOR_TALK_RECORD_START)
    {
      ret = endRecord();
      if (ret == 0)
        setMonitorState(state == MONITOR_RECORD_START ? MONITOR_VIDEO_START : MONITOR_TALK_START);
      else
        ret = VM_ERR_RECORD_END;
    }
    else
    {
      LOG_ERR << "[record]end invalid state:" << MonitorStateToStr(state);
      ret = VM_ERR_RECORD_BAD_STATE;
    }
  }
  else
  {
    LOG_ERR << "[record]invalid operation:" << operation;
    ret = VM_ERR_INVALID_OPERATION;
  }
  ResponseToApp(value, msg, static_cast<uint16_t>(ret));
}

// 0x49a1d8
void MonitorVideo::DeleteVideoControl(MonitorJson& value, MonitorJson& msg)
{
  std::string operation;
  std::string videoName;
  if (!GetMsgParam(value, msg, "operation", "[Delete]", operation, false) ||
      !GetMsgParam(value, msg, "videoName", "[Delete]", videoName, false))
    return;
  int ret = VM_OK;
  const MONITOR_SYS_STATE state = getMonitorState();
  if (operation == "start")
  {
    if (state == MONITOR_SYS_BUTT)
    {
      ret = DeleteFile(videoName);
    }
    else
    {
      LOG_ERR << "[Delete]invalid state:" << MonitorStateToStr(state);
      ret = VM_ERR_BUSY;
    }
  }
  else
  {
    LOG_ERR << "[Delete]invalid operation:" << operation;
    ret = VM_ERR_INVALID_OPERATION;
  }
  ResponseToApp(value, msg, static_cast<uint16_t>(ret));
}

// 0x49a718
void MonitorVideo::RenameVideoControl(MonitorJson& value, MonitorJson& msg)
{
  std::string operation;
  std::string oldName;
  std::string newName;
  if (!GetMsgParam(value, msg, "operation", "[Rename]", operation, false) ||
      !GetMsgParam(value, msg, "old_videoName", "[Rename]", oldName, false) ||
      !GetMsgParam(value, msg, "new_videoName", "[Rename]", newName, false))
    return;
  int ret = VM_OK;
  const MONITOR_SYS_STATE state = getMonitorState();
  if (operation == "start")
  {
    if (state == MONITOR_SYS_BUTT)
    {
      ret = RenameFile(oldName, newName);
    }
    else
    {
      LOG_ERR << "[Rename]invalid state:" << MonitorStateToStr(state);
      ret = VM_ERR_BUSY;
    }
  }
  else
  {
    LOG_ERR << "[Rename]invalid operation:" << operation;
    ret = VM_ERR_INVALID_OPERATION;
  }
  ResponseToApp(value, msg, static_cast<uint16_t>(ret));
}

// 0x49addc — asks the app for an OSS upload URL for an existing file.
void MonitorVideo::DownloadVideoControl(MonitorJson& value, MonitorJson& msg)
{
  int taskid = 0;
  std::string operation;
  std::string videoName;
  if (!GetMsgParam(value, msg, "taskid", "[Download]", taskid, true) ||
      !GetMsgParam(value, msg, "operation", "[Download]", operation, false) ||
      !GetMsgParam(value, msg, "videoName", "[Download]", videoName, false))
    return;
  if (!CheckFileExist(videoName))
  {
    LOG_ERR << "[Download]video file does not exists:" << videoName;
    ResponseToApp(value, msg, VM_ERR_FILE_NOT_EXIST);
    return;
  }
  MonitorJson req;
  req.AddString("method", "oss_upload_url");
  if (taskid == TASK_MP4)
  {
    req.AddInt("taskid", TASK_MP4);
    req.AddString("type", "mp4");
  }
  else if (taskid == TASK_THUMB)
  {
    req.AddInt("taskid", TASK_THUMB_DOWNLOAD);
    req.AddString("type", "thumb");
  }
  else
  {
    LOG_ERR << "[Download]taskid wrong :" << taskid;
    ResponseToApp(value, msg, VM_ERR_BAD_TASKID);
    return;
  }
  req.AddInt("size", GetFileSize(videoName));
  req.AddString("filename", videoName);
  MonitorMsgCenter::GetPtr().SendMsg(req.ToString());
}

// 0x49b844 — the app answers an oss_upload_url request.
void MonitorVideo::UploadVideoControl(MonitorJson& value, MonitorJson& msg)
{
  int taskid = 0;
  if (!GetMsgParam(value, msg, "taskid", "[Upload]", taskid, true))
    return;
  if (taskid != TASK_MP4)
  {
    ResponseToApp(value, msg, VM_ERR_BAD_TASKID);
    return;
  }
  std::string filename;
  long expires = 0;
  if (!GetMsgParam(value, msg, "filename", "[Upload]", filename, false) ||
      !GetMsgParam(value, msg, "expires_time", "[Upload]", expires, false))
    return;
  if (expires == 0 || expires == 1)
  {
    SolveUploadUrlProblem(static_cast<int>(expires), filename);
    return;
  }
  UploadFileMess m;
  long ossId = 0;
  if (!GetMsgParam(value, msg, "url", "[Upload]", m.url, false) ||
      !GetMsgParam(value, msg, "ossId", "[Upload]", ossId, false) ||
      !GetMsgParam(value, msg, "key", "[Upload]", m.key, false))
    return;
  m.filename = filename;
  m.ossId = ossId;
  setOneUploadFileMess(m);
}

// 0x49be7c — operType "video"
void MonitorVideo::ResetVideoControl(MonitorJson& /*unused*/, MonitorJson& /*unused*/)
{
  const uint16_t ret = CloseMonitor("[Reset]");
  ResponseToApp("action", TASK_CAMERA_PROP, "video", "reset", ret, 0, 0);
}

// 0x49c00c
void MonitorVideo::GetPropVideoControl(MonitorJson& value, MonitorJson& msg)
{
  std::string operation;
  if (!GetMsgParam(value, msg, "operation", "[GetProp]", operation, false))
    return;
  MonitorJson reply;
  int status = 0;
  int taskid = 0;
  if (operation == "camera")
  {
    status = getMonitorState();
    taskid = TASK_CAMERA_PROP;
  }
  else
  {
    status = getMMIState();
    taskid = TASK_VIDEO_PROP;
  }
  value.AddInt("result", 666);
  value.AddInt("status", status);
  value.AddString("token", m_token);
  value.AddString("channel", m_channelId);
  reply.AddString("method", "get_properties");
  reply.AddInt("taskid", taskid);
  reply.AddString("value", value.ToString());
  MonitorMsgCenter::GetPtr().SendMsg(reply.ToString());
}

// 0x49c4ec
void MonitorVideo::CheckDiskVideoControl(MonitorJson& value, MonitorJson& /*unused*/)
{
  value.AddInt("result", 666);
  value.AddInt("status", 666);
  MonitorJson disk;
  MonitorJson reply;
  const int total = GetSysConf().video_total_disk_space;
  disk.AddString("operType", "checkdisk");
  disk.AddString("operation", "start");
  disk.AddInt("total", total);
  disk.AddInt("used", total - static_cast<int>(GetDiskFreeSize("")));
  reply.AddString("method", "get_properties");
  reply.AddInt("taskid", TASK_DISK_PROP);
  reply.AddString("value", disk.ToString());
  MonitorMsgCenter::GetPtr().SendMsg(reply.ToString());
}

// 0x49ca30
void MonitorVideo::KeepAliveVideoControl(MonitorJson& /*unused*/, MonitorJson& /*unused*/) { ResetAliveTimer(); }

// 0x49ca54 — tear down whatever is running and go back to idle.
uint16_t MonitorVideo::CloseMonitor(const std::string& tag)
{
  const MONITOR_SYS_STATE state = getMonitorState();
  LOG_INFO << tag << " end monitor state:" << MonitorStateToStr(state);
  MonitorAgoraService& agora = MonitorAgoraService::GetPtr();
  MonitorAudio& audio = MonitorAudio::GetPtr();
  int ret = VM_OK;
  switch (state)
  {
  case MONITOR_SYS_BUTT:
    break;
  case MONITOR_VIDEO_START:
  case MONITOR_TALK_START:
    audio.CloseAudio(true);
    agora.MonitorAgoraDisConnect();
    ret = endMonitor();
    break;
  case MONITOR_RECORD_START:
    endRecord();
    agora.MonitorAgoraDisConnect();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ret = endMonitor();
    break;
  case MONITOR_TALK_RECORD_START:
    endRecord();
    audio.CloseAudio(true);
    agora.MonitorAgoraDisConnect();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ret = endMonitor();
    break;
  case PLAYBACK_VIDEO_START:
    VideoPlaybackStop();
    break;
  default:
    LOG_ERR << tag << " op end but state:" << MonitorStateToStr(state);
    ret = VM_ERR_CLOSE_BAD_STATE;
    break;
  }
  ClearSession();
  setMonitorState(MONITOR_SYS_BUTT);
  return static_cast<uint16_t>(ret);
}

// 0x49cd7c — the app refused/expired the upload URL: report and delete the clip.
void MonitorVideo::SolveUploadUrlProblem(int expires, const std::string& filename)
{
  int result = 0;
  if (expires == 0)
    result = 1;
  else if (expires == 1)
    result = 2;
  MonitorMsgCenter::GetPtr().SendMsg(
      JsonForResponseUpload(TASK_MP4, 0, result, filename, "mp4", "", getMonitorState(), 0).ToString());
  std::string base;
  if (const size_t dot = filename.find_last_of('.'); dot != std::string::npos)
    base = filename.substr(0, dot);
  std::remove((base + ".mp4").c_str());
  std::remove((base + ".jpg").c_str());
}

// 0x49cfe8
bool MonitorVideo::getOneUploadFileMess(UploadFileMess& out)
{
  std::scoped_lock const lock(m_uploadMutex);
  if (m_uploadQueue.empty())
    return false;
  out = m_uploadQueue.front();
  m_uploadQueue.pop();
  return true;
}

// 0x49d08c — at most 101 pending uploads.
void MonitorVideo::setOneUploadFileMess(const UploadFileMess& m)
{
  std::scoped_lock const lock(m_uploadMutex);
  if (m_uploadQueue.size() <= 100)
    m_uploadQueue.push(m);
}

// 0x49d114 — mp4 (AES-encrypted in place) first, then the matching jpg.
void MonitorVideo::UploadFile2Sever()
{
  SetThreadName("UploadFile2Sever");
  for (;;)
  {
    usleep(1000);
    UploadFileMess m;
    if (!getOneUploadFileMess(m))
      continue;
    int fileSize = 0;
    if (m.filename.find("mp4") != std::string::npos)
      EncryptFile(m.filename, m.key);
    const bool ok = UploadFileByCurl(m.url, m.filename, vmpath::CA_CERT_DIR, fileSize);
    SolveUploadFileProblem(ok, m.ossId, m.filename, m.key, fileSize);
  }
}

// 0x49d29c
void MonitorVideo::SolveUploadFileProblem(bool ok, long ossId, const std::string& filename, const std::string& key,
                                          int fileSize)
{
  MonitorMsgCenter& mc = MonitorMsgCenter::GetPtr();
  const size_t dot = filename.find_last_of('.');
  if (dot == std::string::npos)
  {
    mc.SendMsg(JsonForResponseUpload(TASK_MP4, ossId, 1, filename, "mp4", key, getMonitorState(), fileSize).ToString());
    std::remove(filename.c_str());
    LOG_ERR << "[SolveUploadFileProblem]upload " << "" << " faile";
    return;
  }
  const std::string base = filename.substr(0, dot);
  const std::string ext = filename.substr(dot + 1);
  const std::string mp4 = base + ".mp4";
  const std::string jpg = base + ".jpg";
  if (!ok)
  {
    LOG_ERR << "[SolveUploadFileProblem]upload " << ext << " faile";
    mc.SendMsg(JsonForResponseUpload(TASK_MP4, ossId, 1, filename, "mp4", key, getMonitorState(), fileSize).ToString());
    std::remove(mp4.c_str());
    std::remove(jpg.c_str());
    return;
  }
  if (ext.find("jpg") == std::string::npos)
  {
    // mp4 done -> now request a URL for the thumbnail.
    EncryptFile(jpg, key);
    mc.SendMsg(JsonForUploadUrl("thumb", jpg, key, 0, TASK_MP4).ToString());
  }
  else
  {
    const long mp4Size = GetFileSize(mp4);
    mc.SendMsg(JsonForResponseUpload(TASK_MP4, ossId, 0, filename, "thumb", key, getMonitorState(),
                                     static_cast<int>(mp4Size) + fileSize)
                   .ToString());
    std::remove(mp4.c_str());
    std::remove(jpg.c_str());
  }
}

// monitor_common — global state, system config, session bookkeeping, file/crypto helpers and
// the JSON response builders shared by every module.
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "monitor_json.h"

// Values double as the "status" field reported to the app.
enum MONITOR_SYS_STATE : int
{
  MONITOR_SYS_BUTT = 0, // idle
  MONITOR_VIDEO_START = 1,
  MONITOR_VIDEO_END = 2,
  MONITOR_TALK_START = 3,
  MONITOR_TALK_END = 4,
  MONITOR_RECORD_START = 5,
  MONITOR_RECORD_END = 6,
  MONITOR_TALK_RECORD_START = 7,
  MONITOR_TALK_RECORD_END = 8,
  PLAYBACK_VIDEO_START = 9,
  PLAYBACK_VIDEO_END = 10,
};

enum MMI_SYS_STATE : int
{
  MMI_SYS_IDLE = 0,
};

// Internal (non-JSON) messages exchanged with the "ava" daemon: "$<id>:<payload>".
enum INTERNAL_MSG_ID : int
{
  INTER_MSG_INTERRUPT_SPEAKER = 0, // ava -> vm
  INTER_MSG_MONITOR_STATE = 1,     // vm -> ava, payload = MONITOR_SYS_STATE
  INTER_MSG_SPEAKER_RESUMED = 2,   // vm -> ava
  INTER_MSG_RELOAD_LOG_CFG = 4,    // ava -> vm, payload (from index 3) = log_switch.json path
};

// Result codes returned in "result".
enum MonitorErr : uint16_t
{
  VM_OK = 0,
  VM_ERR_FILE_NOT_EXIST = 0x1000,
  VM_ERR_FILE_EXIST = 0x1001,
  VM_ERR_FILE_OPEN = 0x1002,
  VM_ERR_FILE_STAT = 0x1003,
  VM_ERR_CURL = 0x1004,
  VM_ERR_AGORA_INIT = 0x2000,
  VM_ERR_AGORA_CONNECT = 0x2001,
  VM_ERR_RECORDER_START = 0x2400,
  VM_ERR_SPEAKER_INIT = 0x2601,
  VM_ERR_CLOSE_BAD_STATE = 0x3010,
  VM_ERR_INVALID_OPERATION = 0x3020,
  VM_ERR_RECORD_BAD_STATE = 0x3029,
  VM_ERR_BUSY = 0x3030,
  VM_ERR_PLAYBACK_BAD_STATE = 0x3033,
  VM_ERR_RECORD_START = 0x3034,
  VM_ERR_RECORD_END = 0x3035,
  VM_ERR_MISSING_PARAM = 0x3100,
  VM_ERR_BAD_TASKID = 0x3101,
  VM_ERR_UNKNOWN_OPERTYPE = 0x3102,
};

// Task ids used in the miio-style "properties_changed"/"action" messages.
enum MonitorTaskId : int
{
  TASK_MONITOR = 1,
  TASK_INTERCOM = 2,
  TASK_THUMB = 4,
  TASK_MP4 = 1002,
  TASK_THUMB_DOWNLOAD = 1004,
  TASK_CAMERA_PROP = 2001,
  TASK_VIDEO_PROP = 2002,
  TASK_DISK_PROP = 2003,
};

// SYS_CONF (0xa8 bytes), filled from video_monitor.cfg.
struct SysCfgParamType
{
  std::string version;            // +0x00
  std::string appid;              // +0x20
  int camera_id = 0;              // +0x40
  std::string video_filepath;     // +0x48
  int video_total_disk_space = 0; // +0x68 (MiB)
  int video_file_size = 0;        // +0x6c
  int video_least_file_size = 0;  // +0x70
  int video_least_disk_space = 0; // +0x74
  int enable_catch_signal = 0;    // +0x78
  std::string recorder_cfg_path;  // +0x80
  int enable_save_app_audio = 0;  // +0xa0
  int enable_save_video_h264 = 0; // +0xa4
};

struct H264Buffer
{ // 0x18
  int32_t size = 0;
  bool isKeyFrame = false;
  int64_t reserved = 0;
  uint8_t* data = nullptr; // malloc'd, consumer frees
};

struct AudioDataFromAgora
{                              // 0x10
  void* audioBuffer = nullptr; // malloc'd
  size_t size = 0;
};

extern const char* CRYPT_KEY;
extern const double ZERO_E;
extern int video_width;
extern int video_rest;

// --- state -----------------------------------------------------------------------------------
std::string MonitorStateToStr(MONITOR_SYS_STATE s);
MONITOR_SYS_STATE getMonitorState();
MMI_SYS_STATE getMMIState();
void setMonitorState(MONITOR_SYS_STATE s);
void setMMIState(MMI_SYS_STATE s);

// --- heartbeat / sessions --------------------------------------------------------------------
void ResetHeartBeatSwitch();
void ResetAliveTimer();
bool CheckTimeValid(const time_t& t);
bool CheckTimeoutDead();
void ClearSession();
void UpdateSession(const std::string& session);
void CheckSession();
bool IsSessionEmpty();

// --- config ----------------------------------------------------------------------------------
bool VmSetConfigPath(const std::string& path);
bool InitSysConf();
bool InitVideoResolution();
SysCfgParamType& GetSysConf();
std::string GetRecorderCfgPath();
bool GetSaveAppAudioFlag();
bool GetSaveH264Flag();
int GetVideoResolution(); // height
int GetVideoWidth();
int GetVideoHeight();
int GetDeviceDid();
bool GetAreaCode(std::string& area);

// --- misc helpers ----------------------------------------------------------------------------
long GetCpuTime(); // CLOCK_MONOTONIC in microseconds
std::string GetKeyVal(const std::string& src, const std::string& key);
bool CheckZero(double v);
bool SaveAudio(const AudioDataFromAgora& data);
bool SaveH264(const H264Buffer& buf);
std::string GetUTCTimeStr();
std::string getCurrentSystemTimeChrono(); // "YYYYmmddHHMMSSmmm"
bool CheckVmIsAlreadyRun(int selfCount);
time_t String2Time_t(const std::string& s);
bool ReadFile(const std::string& path, std::string& out);

bool CheckFileExist(const std::string& path);
int DeleteFile(const std::string& name);
int RenameFile(const std::string& oldName, const std::string& newName);
bool EncryptFile(const std::string& path, const std::string& key);
bool DecryptFile(const std::string& path);
unsigned GetDiskFreeSize(const std::string& path); // MiB
long GetFileSize(const std::string& path);

bool Md5sumString_(const unsigned char* data, int len, std::string& out);
bool getRoundData(int bytes, std::string& out);
bool EncryptData(const std::string& plain, const std::string& key, const std::string& iv, std::string& out);

// --- JSON helpers ----------------------------------------------------------------------------
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, std::string& out);
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, int& out);
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, long& out);
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, unsigned& out);

bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, const std::string& tag, std::string& out,
                 bool fromMsg);
bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, const std::string& tag, int& out,
                 bool fromMsg);
bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, const std::string& tag, long& out,
                 bool fromMsg);
bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, int& out, bool fromMsg);

std::string MakeInterMsg(INTERNAL_MSG_ID id, const std::string& payload);
void ResponseToAva(const std::string& msg);
void ResponseToApp(MonitorJson& value);
void ResponseToApp(MonitorJson& value, MonitorJson& msg, uint16_t result);
void ResponseToApp(const std::string& method, int taskid, const std::string& operType, const std::string& operation,
                   int result, int status, long ossId);
void ResponseToApp(int taskid, const std::string& operType, const std::string& operation);
void ResponseOpState(const std::string& what);
void ResponseAgoraFail();

MonitorJson JsonForUploadUrl(const std::string& type, const std::string& filename, const std::string& key, int category,
                             int taskid);
MonitorJson JsonForResponseUpload(int taskid, long ossId, int result, const std::string& filename,
                                  const std::string& type, const std::string& key, int status, int filesize);
MonitorJson JsonForValue(const std::string& operType, const std::string& operation, int result, int status);

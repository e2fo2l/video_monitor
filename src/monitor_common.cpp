#include "monitor_common.h"
#include "monitor_json.h"
#include "rapidjson/document.h"
#include "rapidjson/reader.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <iterator>
#include <map>
#include <sched.h>
#include <sstream>

#include <string>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#if !VM_COMPAT
#include <filesystem>
#include <system_error>
#endif

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <rapidjson/filereadstream.h>

#include "monitor_audio.h"
#include "monitor_msg_center.h"
#include "ms_log.h"
#include "util/md5.h"
#include "util/xxtea.h"
#include "vm_paths.h"

const char* CRYPT_KEY = "123456789";
const double ZERO_E = 0.001;
int video_width = 864; // encoder output width (recorder cfg)
int video_rest = 480;  // encoder output height (recorder cfg)

namespace
{

// DAT_0053f7f8. A function-local static, so a failing allocation cannot throw before main().
std::string& ConfigPath()
{
  static std::string path = vmpath::VIDEO_MONITOR_CFG;
  return path;
}
std::atomic<MONITOR_SYS_STATE> g_monitor_state{MONITOR_SYS_BUTT};
std::atomic<MMI_SYS_STATE> g_mmi_state{MMI_SYS_IDLE};
SysCfgParamType SYS_CONF;
bool g_heartbeat_switch = false;
std::map<std::string, long> g_SessionMap;
int g_responseCounter = 0; // "df" field, DAT_0053f800

constexpr std::array<const char*, 11> g_monitor_state_list = {
    "MONITOR_SYS_BUTT",        "MONITOR_VIDEO_START",  "MONITOR_VIDEO_END",  "MONITOR_TALK_START",
    "MONITOR_TALK_END",        "MONITOR_RECORD_START", "MONITOR_RECORD_END", "MONITOR_TALK_RECORD_START",
    "MONITOR_TALK_RECORD_END", "PLAYBACK_VIDEO_START", "PLAYBACK_VIDEO_END",
};

} // namespace

// ============================================================================================
// state
// ============================================================================================

// 0x4765f0
std::string MonitorStateToStr(MONITOR_SYS_STATE s)
{
  if (s < 0 || static_cast<size_t>(s) >= g_monitor_state_list.size())
    return "";
  // Range-checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return g_monitor_state_list[s];
}

MONITOR_SYS_STATE getMonitorState() { return g_monitor_state; } // 0x4766b4
MMI_SYS_STATE getMMIState() { return g_mmi_state; }             // 0x4766d0

// 0x4766ec — every state change is mirrored to ava as "$1:<state>".
void setMonitorState(MONITOR_SYS_STATE s)
{
  g_monitor_state = s;
  ResponseToAva(MakeInterMsg(INTER_MSG_MONITOR_STATE, std::to_string(s)));
}

void setMMIState(MMI_SYS_STATE s) { g_mmi_state = s; } // 0x476780

// ============================================================================================
// heartbeat / sessions
// ============================================================================================

// 0x4767a8 — called for every message received from ava.
void ResetHeartBeatSwitch()
{
  if (!g_heartbeat_switch)
    ResetAliveTimer();
}

void ResetAliveTimer() { g_heartbeat_switch = true; } // 0x479814

// 0x47982c — guards against the RTC not being set yet (1970).
bool CheckTimeValid(const time_t& t)
{
  std::tm tm{};
  localtime_r(&t, &tm);
  return tm.tm_year + 1900 >= 2000;
}

// 0x47986c — true once every app session has expired; main() then tears the stream down.
bool CheckTimeoutDead()
{
  if (!g_heartbeat_switch)
    return false;
  CheckSession();
  if (!g_SessionMap.empty())
    return false;
  g_heartbeat_switch = false;
  return true;
}

void ClearSession() { g_SessionMap.clear(); } // 0x47cd7c

// 0x47cd9c
void UpdateSession(const std::string& session) { g_SessionMap[session] = time(nullptr); }

// 0x47ce60 — sessions expire after 60 s without a message.
void CheckSession()
{
  for (auto it = g_SessionMap.begin(); it != g_SessionMap.end();)
  {
    if (time(nullptr) - it->second <= 60)
    {
      ++it;
      continue;
    }
    if (!CheckTimeValid(it->second))
    {
      it->second = time(nullptr); // clock jumped (NTP sync); restart the timer
      ++it;
      continue;
    }
    const std::string expired = it->first;
    it = g_SessionMap.erase(it);
    MonitorAudio& audio = MonitorAudio::GetPtr();
    if (audio.IsAudioOpen() && expired == audio.getAudioSession())
    {
      audio.CloseAudio(false);
      LOG_ERR << "[CheckSession] Overdue Session is " << expired;
    }
  }
}

bool IsSessionEmpty() { return g_SessionMap.empty(); } // 0x47d0dc

// ============================================================================================
// config
// ============================================================================================

// 0x476ba0
bool VmSetConfigPath(const std::string& path)
{
  if (!path.empty())
    ConfigPath() = path;
  return true;
}

// 0x476be8
bool InitSysConf()
{
  // NOLINTBEGIN(cppcoreguidelines-owning-memory): rapidjson's FileReadStream reads from a C stdio
  // handle, closed right below.
  FILE* fp = fopen(ConfigPath().c_str(), "rb");
  if (!fp)
  {
    LOG_ERR << "[common]open sys config file failed!";
    return false;
  }
  char buf[4096];
  rapidjson::FileReadStream is(fp, buf, sizeof buf);
  // NOTE(orig): the file is closed before parsing; FileReadStream already buffered the first
  // 4 KiB, so configs larger than that are truncated.
  fclose(fp);
  // NOLINTEND(cppcoreguidelines-owning-memory)

  MonitorJson doc;
  doc.ParseStream<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(is);
  if (doc.HasParseError())
  {
    LOG_ERR << "[common]sys config json parse err!";
    return false;
  }
  if (!doc.IsObject())
  {
    LOG_ERR << "[common]sys config format err!";
    return false;
  }

  const std::string tag = "[common]";
  SysCfgParamType& c = SYS_CONF;
  if (!GetJsonParam(doc, "version", tag, c.version) || !GetJsonParam(doc, "appid", tag, c.appid) ||
      !GetJsonParam(doc, "camera_id", tag, c.camera_id) ||
      !GetJsonParam(doc, "video_filepath", tag, c.video_filepath) ||
      !GetJsonParam(doc, "video_total_disk_space", tag, c.video_total_disk_space) ||
      !GetJsonParam(doc, "video_file_size", tag, c.video_file_size) ||
      !GetJsonParam(doc, "video_least_file_size", tag, c.video_least_file_size) ||
      !GetJsonParam(doc, "video_least_disk_space", tag, c.video_least_disk_space) ||
      !GetJsonParam(doc, "enable_catch_signal", tag, c.enable_catch_signal))
    return false;

  // Optional keys.
  GetJsonParam(doc, "recorder_cfg_path", tag, c.recorder_cfg_path);
  GetJsonParam(doc, "enable_save_app_audio", tag, c.enable_save_app_audio);
  GetJsonParam(doc, "enable_save_video_h264", tag, c.enable_save_video_h264);
  InitVideoResolution();
  return true;
}

// 0x477f64 — reads "encoder_voutput_width = W" / "encoder_voutput_height = H" from the
// TRecorder cfg and maps the pair to one of the hard-coded SPS/PPS tables (see monitor_record).
bool InitVideoResolution()
{
  std::ifstream in(SYS_CONF.recorder_cfg_path);
  const std::string cfg{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

  size_t pos = cfg.find("encoder_voutput_width", 0);
  if (pos == std::string::npos)
    return false;
  pos = cfg.find('=', pos);
  if (pos == std::string::npos)
    return false;
  // NOTE(orig): std::stoi throws on garbage -> std::terminate.
  const int width = std::stoi(cfg.substr(pos + 1), nullptr, 10);

  size_t hpos = cfg.find("encoder_voutput_height", pos);
  if (hpos == std::string::npos)
    return false;
  hpos = cfg.find('=', hpos);
  if (hpos == std::string::npos)
    return false;
  const int height = std::stoi(cfg.substr(hpos + 1), nullptr, 10);

#if VM_COMPAT
  // NOTE(orig): only these five sizes had an SPS/PPS table; anything else silently kept
  // 864x480, so a 1080p encoder got 864x480 headers spliced into its stream.
  const bool known = (width == 672 && height == 504) || (width == 240 && height == 180) ||
                     (width == 480 && height == 360) || (width == 864 && height == 480) ||
                     (width == 1280 && height == 720);
#else
  // The SPS/PPS is generated from the size (see util/h264_param_sets), so any size works.
  const bool known = width > 0 && height > 0;
#endif
  if (known)
  {
    video_width = width;
    video_rest = height;
  }

  LOG_INFO << "[InitVideoResolution]rest:" << video_width << "x" << video_rest;
  return true;
}

SysCfgParamType& GetSysConf() { return SYS_CONF; }                         // 0x478344
std::string GetRecorderCfgPath() { return SYS_CONF.recorder_cfg_path; }    // 0x476990
bool GetSaveAppAudioFlag() { return SYS_CONF.enable_save_app_audio == 1; } // 0x4769c4
bool GetSaveH264Flag() { return SYS_CONF.enable_save_video_h264 == 1; }    // 0x4769e0
int GetVideoResolution() { return video_rest; }                            // 0x47b850
int GetVideoWidth() { return video_width; }
int GetVideoHeight() { return video_rest; }

// 0x479670 — "did=<n>" from device.conf; used as the Agora uid.
int GetDeviceDid()
{
  int did = -1;
  std::ifstream in(vmpath::DEVICE_CONF);
  if (!in)
  {
    LOG_ERR << "[GetDeviceDid] open file fail!";
    return did;
  }
  std::string line;
  while (std::getline(in, line))
  {
    const size_t pos = line.find("did=");
    if (pos != std::string::npos)
    {
      // atoi() in the original, which is (int)strtol() on glibc.
      // NOTE(orig): DIDs above INT_MAX are truncated
      did = static_cast<int>(std::strtol(line.c_str() + pos + 4, nullptr, 10));
      break;
    }
  }
  return did;
}

// 0x47c87c — region string ("cn", "eu", ...) from the dreame or miio config tree.
bool GetAreaCode(std::string& area)
{
  std::string flag;
  if (!ReadFile(vmpath::IOT_FLAG, flag))
    return false;
  if (flag.empty())
  {
    LOG_ERR << "[GetAreaCode] open " << vmpath::IOT_FLAG << " value is empty";
    return false;
  }
  const std::string regionFile = (flag == "dmiot") ? vmpath::DMIO_DEVICE_REGION : vmpath::MIIO_DEVICE_COUNTRY;
  if (!ReadFile(regionFile, area))
    return false;
  if (area.empty())
  {
    LOG_ERR << "[GetAreaCode] open " << regionFile << " value is empty";
    return false;
  }
  if (flag == "miiot" && area == "\n")
  {
    area = "cn";
    return true;
  }
  LOG_ERR << "[GetAreaCode] Area is " << area;
  return true;
}

// ============================================================================================
// misc helpers
// ============================================================================================

// 0x4767e0
long GetCpuTime()
{
  // CLOCK_MONOTONIC, in microseconds.
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 0x476838
std::string GetKeyVal(const std::string& src, const std::string& key)
{
  const size_t pos = src.find(key);
  if (pos == std::string::npos)
    return "";
  return src.substr(pos + key.length(), src.length() - key.length() - pos);
}

bool CheckZero(double v) { return v < ZERO_E && v > -ZERO_E; } // 0x476918

// 0x4769fc — debug dump of the intercom audio received from the app.
bool SaveAudio(const AudioDataFromAgora& data)
{
  static bool first = true;
  if (!GetSaveAppAudioFlag())
    return false;
  if (first)
  {
    std::remove(vmpath::DEBUG_AUDIO_DUMP);
    first = false;
  }
  if (std::ofstream out(vmpath::DEBUG_AUDIO_DUMP, std::ios::binary | std::ios::app); out)
    out.write(static_cast<const char*>(data.audioBuffer), static_cast<std::streamsize>(data.size));
  else
    std::cout << "open rcv_audio.pcm fail\n";
  return true;
}

// 0x476acc — debug dump of the exact H.264 byte stream handed to Agora
// ("enable_save_video_h264": 1 in video_monitor.cfg).
bool SaveH264(const H264Buffer& buf)
{
  static bool first = true;
  if (!GetSaveH264Flag())
    return false;
  if (first)
  {
    std::remove(vmpath::DEBUG_H264_DUMP);
    first = false;
  }
  if (std::ofstream out(vmpath::DEBUG_H264_DUMP, std::ios::binary | std::ios::app); out)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): ostream::write takes the bytes as char
    out.write(reinterpret_cast<const char*>(buf.data), static_cast<std::streamsize>(buf.size));
  else
    std::cout << "open send_h264.h264 fail\n";
  return true;
}

// 0x47b528 — "YYYYmmdd-HHMMSS.mmm" (local time despite the name)
std::string GetUTCTimeStr()
{
  timeval tv{};
  gettimeofday(&tv, nullptr);
  const time_t t = tv.tv_sec;
  std::tm tm{};
  localtime_r(&t, &tm);
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << tm.tm_year + 1900 << std::setw(2) << tm.tm_mon + 1 << std::setw(2)
      << tm.tm_mday << '-' << std::setw(2) << tm.tm_hour << std::setw(2) << tm.tm_min << std::setw(2) << tm.tm_sec
      << '.' << std::setw(3) << tv.tv_usec / 1000;
  return out.str();
}

#if VM_COMPAT
// 0x47b640 — single-instance check.
//
// NOTE(orig): this is the main reason the binary refuses to start on newer firmware. It counts
// *substring occurrences* of "video_monitor" in `ps aux` output, not processes. Started as
//     /ava/bin/video_monitor -f /ava/conf/video_monitor/video_monitor.cfg
// its own command line already contains the word three times. It only works on the stock
// firmware because busybox `ps` truncates every line to ~80 columns (79 when stdin is not a
// tty), which cuts the cfg path off. Any `ps` that prints full command lines (procps-ng,
// toybox, newer busybox, or a wide terminal on stdin) makes the process see itself and exit
// with "video_monitor is Running, Please Check". The loop also fgets() into a 256-byte stack
// buffer with no bound, so long `ps` output smashes the stack.
bool CheckVmIsAlreadyRun(int selfCount)
{
  const std::string cmd = R"(ps aux | grep "video_monitor" | grep -vE "grep|gdb")";
  char out[256] = {};
  int len = 0;
  // NOLINTNEXTLINE(cert-env33-c,bugprone-command-processor,cppcoreguidelines-owning-memory): faithful to the original
  FILE* fp = popen(cmd.c_str(), "r");
  while (!feof(fp))
  {
    fgets(out + len, 0x40, fp); // unbounded: overflows `out` past 4 lines
    len = static_cast<int>(strlen(out));
  }
  pclose(fp); // NOLINT(cppcoreguidelines-owning-memory): see popen() above

  const std::string psOut = out;
  const std::string needle = "video_monitor";
  int hits = 0;
  for (size_t p = psOut.find(needle); p != std::string::npos; p = psOut.find(needle, p + needle.length()))
    ++hits;
  return hits >= selfCount + 1;
}
#else
// Fixed variant: count other processes whose executable basename is video_monitor.
bool CheckVmIsAlreadyRun(int /*selfCount*/)
{
  const pid_t self = getpid();
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec))
  {
    const std::string name = entry.path().filename().string();
    const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
    if (pid <= 0 || pid == self)
      continue;
    std::ifstream cmdline(entry.path() / "cmdline");
    std::string argv0;
    std::getline(cmdline, argv0, '\0');
    if (std::filesystem::path(argv0).filename() == "video_monitor")
      return true;
  }
  return false;
}
#endif

// 0x47c5d4
time_t String2Time_t(const std::string& s)
{
  std::tm tm{};
  std::istringstream ss(s);
  ss >> std::get_time(&tm, "%Y%m%d%H%M%S");
  return mktime(&tm);
}

// 0x47c67c
bool ReadFile(const std::string& path, std::string& out)
{
  std::ifstream in;
  in.open(path);
  if (!in.is_open())
  {
    LOG_ERR << "[ReadFile]VerifyLicense open " << path << " faild";
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  in.close();
  return true;
}

// ============================================================================================
// files / crypto
// ============================================================================================

// 0x47846c
bool CheckFileExist(const std::string& path) { return !path.empty() && access(path.c_str(), F_OK) == 0; }

// 0x4784b8
int DeleteFile(const std::string& name)
{
  const std::string path = SYS_CONF.video_filepath + name;
  if (access(path.c_str(), F_OK) == -1)
  {
    LOG_ERR << "[delete]file not exists:" << path;
    return VM_ERR_FILE_NOT_EXIST;
  }
  return std::remove(path.c_str());
}

// 0x478628
int RenameFile(const std::string& oldName, const std::string& newName)
{
  const std::string from = SYS_CONF.video_filepath + oldName;
  const std::string to = SYS_CONF.video_filepath + newName;
  if (access(from.c_str(), F_OK) == -1)
  {
    LOG_ERR << "[rename]file not exists:" << from;
    return VM_ERR_FILE_NOT_EXIST;
  }
  if (access(to.c_str(), F_OK) == 0)
  {
    LOG_ERR << "[rename]file already exists:" << to;
    return VM_ERR_FILE_EXIST;
  }
  return rename(from.c_str(), to.c_str());
}

// 0x4788e8 — AES-256-CBC in place; key = md5hex(key), fixed IV.
bool EncryptFile(const std::string& path, const std::string& key)
{
  std::ifstream in;
  in.open(path);
  if (!in.is_open())
  {
    LOG_ERR << "[EncryptFile]EncryptFile open " << path << " faild";
    return false;
  }
  const std::string plain{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  in.close();

  const std::string tmpPath = path + ".tmp";
  const std::string iv = "aebf8c52e26a4767";
  std::string cipher;
  std::string aesKey;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the C hashing API takes bytes as unsigned char
  if (!Md5sumString_(reinterpret_cast<const unsigned char*>(key.c_str()), static_cast<int>(key.size()), aesKey))
  {
    LOG_ERR << "[EncryptFile]Md5sumString_  faild";
    return false;
  }
  if (!EncryptData(plain, aesKey, iv, cipher))
  {
    LOG_ERR << "[EncryptFile]EncryptData  faild";
    return false;
  }
  std::ofstream out;
  out.open(tmpPath, std::ios::out);
  if (!out.is_open())
  {
    LOG_ERR << "[EncryptFile]tmp_file_path  open " << tmpPath << " failed";
    return false;
  }
  out << cipher;
  out.close();
  std::remove(path.c_str());
  rename(tmpPath.c_str(), path.c_str());
  return true;
}

// 0x478f1c — xxtea("123456789") in place.
// NOTE(orig): EncryptFile uses AES, so files encrypted by this program never decrypt here.
bool DecryptFile(const std::string& path)
{
  std::ifstream in(path);
  const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  size_t plainLen = 0;
  const void* plain = xxtea_decrypt(data.c_str(), data.size(), CRYPT_KEY, &plainLen);
  if (!plain)
  {
    LOG_ERR << "[decrypt] decrypt fail!";
    return false;
  }
  const std::string tmpPath = SYS_CONF.video_filepath + "tmp.mp4";
  std::ofstream out(tmpPath, std::ios::binary);
  out.write(static_cast<const char*>(plain), static_cast<std::streamsize>(plainLen));
  const auto written = out ? plainLen : 0;
  out.close();
  if (written < plainLen)
  {
    LOG_ERR << "[decrypt] write file fail! file_len:" << plainLen << " write_len:" << written;
    std::remove(tmpPath.c_str());
    return false; // NOTE(orig): `plain` leaks
  }
  std::remove(path.c_str());
  rename(tmpPath.c_str(), path.c_str());
  return true; // NOTE(orig): `plain` leaks
}

// 0x4792d0
unsigned GetDiskFreeSize(const std::string& path)
{
  const std::string p = path.empty() ? SYS_CONF.video_filepath : path;
  struct statfs st{};
  if (statfs(p.c_str(), &st) == -1)
  {
    LOG_ERR << "[GetDiskFreeSize]statfs fail!";
    return 0xffffffffU;
  }
  return static_cast<unsigned>((st.f_bsize * st.f_bavail) >> 20);
}

// 0x47945c
long GetFileSize(const std::string& path)
{
  if (path.empty())
  {
    LOG_ERR << "[GetFileSize]file_name empty!";
    return 0;
  }
  struct stat st{};
  if (stat(path.c_str(), &st) < 0)
  {
    LOG_ERR << "[GetFileSize]stat fail!" << path;
    return 0;
  }
  return st.st_size;
}

// 0x47c208
bool Md5sumString_(const unsigned char* data, int len, std::string& out)
{
  if (!data)
    return false;
  unsigned char digest[16] = {};
  MD5_CTX ctx{};
  MD5Init(&ctx);
  MD5Update(&ctx, data, static_cast<unsigned>(len));
  MD5Final(&ctx, digest);
  constexpr char kHex[] = "0123456789abcdef";
  out.clear();
  for (const unsigned char b : digest)
  {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): nibbles, 0..15
    out += kHex[b >> 4];
    out += kHex[b & 0xf];
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  }
  return true;
}

// 0x47c2ec — random hex string.
// NOTE(orig): keeps only BN_num_bytes() characters of the 2*bytes long hex string.
bool getRoundData(int bytes, std::string& out)
{
  if (bytes < 1)
    return false;
  BIGNUM* rnd = BN_new();
  if (!rnd)
    return false;
  if (BN_rand(rnd, bytes * 8, -1, 1) == 0)
  {
    BN_free(rnd);
    return false;
  }
  char* hex = BN_bn2hex(rnd);
  if (!hex)
  {
    BN_free(rnd);
    return false;
  }
  out.assign(hex, static_cast<size_t>((BN_num_bits(rnd) + 7) / 8));
  OPENSSL_free(hex);
  BN_free(rnd);
  return true;
}

// 0x47c3f0
bool EncryptData(const std::string& plain, const std::string& key, const std::string& iv, std::string& out)
{
  const EVP_CIPHER* cipher = EVP_aes_256_cbc();
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  EVP_CIPHER_CTX_reset(ctx);
  EVP_CIPHER_CTX_set_padding(ctx, 1);

  bool ok = false;
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): OpenSSL takes and returns bytes as
  // unsigned char, the strings hold them as char.
  if (EVP_CipherInit_ex(ctx, cipher, nullptr, reinterpret_cast<const unsigned char*>(key.c_str()),
                        reinterpret_cast<const unsigned char*>(iv.c_str()), 1) == 1)
  {
    std::vector<unsigned char> buf(plain.size() + 100);
    int len = 0;
    int finLen = 0;
    if (EVP_CipherUpdate(ctx, buf.data(), &len, reinterpret_cast<const unsigned char*>(plain.c_str()),
                         static_cast<int>(plain.size())) == 1 &&
        EVP_CipherFinal_ex(ctx, buf.data() + len, &finLen) == 1)
    {
      out.assign(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(len) + static_cast<size_t>(finLen));
      ok = true;
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  EVP_CIPHER_CTX_reset(ctx);
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

// ============================================================================================
// JSON helpers
// ============================================================================================

namespace
{
template <typename T, typename Is, typename Get>
bool getJson(const MonitorJson& j, const std::string& key, const std::string& tag, T& out, Is is, Get get)
{
  if (const auto it = j.FindMember(key.c_str()); it != j.MemberEnd() && is(it->value))
  {
    out = get(it->value);
    return true;
  }
  LOG_ERR << tag << " data do not have " << key;
  return false;
}

template <typename T, typename Is, typename Get>
bool getMsg(MonitorJson& value, MonitorJson& msg, const std::string& key, const std::string& tag, T& out, bool fromMsg,
            Is is, Get get)
{
  MonitorJson& j = fromMsg ? msg : value;
  if (const auto it = j.FindMember(key.c_str()); it != j.MemberEnd() && is(it->value))
  {
    out = get(it->value);
    return true;
  }
  LOG_ERR << tag << " msg do not have " << key;
  ResponseToApp(value, msg, VM_ERR_MISSING_PARAM);
  return false;
}

const auto isStr = [](const rapidjson::Value& v) { return v.IsString(); };
const auto getStr = [](const rapidjson::Value& v) { return std::string(v.GetString()); };
const auto isInt = [](const rapidjson::Value& v) { return v.IsInt(); };
const auto getInt = [](const rapidjson::Value& v) { return v.GetInt(); };
const auto isI64 = [](const rapidjson::Value& v) { return v.IsInt64(); };
const auto getI64 = [](const rapidjson::Value& v) { return static_cast<long>(v.GetInt64()); };
} // namespace

// 0x4798d4 / 0x479a70 / 0x479c0c / 0x479da8
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, std::string& out)
{
  return getJson(j, key, tag, out, isStr, getStr);
}
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, int& out)
{
  return getJson(j, key, tag, out, isInt, getInt);
}
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, long& out)
{
  return getJson(j, key, tag, out, isI64, getI64);
}
bool GetJsonParam(const MonitorJson& j, const std::string& key, const std::string& tag, unsigned& out)
{
  return getJson(
      j, key, tag, out, [](const rapidjson::Value& v) { return v.IsUint(); },
      [](const rapidjson::Value& v) { return v.GetUint(); });
}

// 0x479f44 / 0x47a124 / 0x47a304
bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, const std::string& tag, std::string& out,
                 bool fromMsg)
{
  return getMsg(value, msg, key, tag, out, fromMsg, isStr, getStr);
}
bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, const std::string& tag, int& out,
                 bool fromMsg)
{
  return getMsg(value, msg, key, tag, out, fromMsg, isInt, getInt);
}
bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, const std::string& tag, long& out,
                 bool fromMsg)
{
  return getMsg(value, msg, key, tag, out, fromMsg, isI64, getI64);
}

// 0x47a4e4 — optional int, silent on absence.
bool GetMsgParam(MonitorJson& value, MonitorJson& msg, const std::string& key, int& out, bool fromMsg)
{
  MonitorJson& j = fromMsg ? msg : value;
  const auto it = j.FindMember(key.c_str());
  if (it == j.MemberEnd() || !it->value.IsInt())
    return false;
  out = static_cast<int>(it->value.GetInt64());
  return true;
}

// 0x478350 — "$<id>:<payload>"
std::string MakeInterMsg(INTERNAL_MSG_ID id, const std::string& payload)
{
  return std::string(1, '$') + std::to_string(id) + ":" + payload;
}

// 0x47b504
void ResponseToAva(const std::string& msg) { MonitorMsgCenter::GetPtr().SendMsg(msg); }

// 0x47a5d0 — unsolicited event: {"value": "<value json>"}
void ResponseToApp(MonitorJson& value)
{
  MonitorJson outer;
  outer.AddString("value", value.ToString());
  MonitorMsgCenter::GetPtr().SendMsg(outer.ToString());
}

// 0x47a6d8 — reply to a request: augments `value` with result/status/df and sends it back
// inside the original envelope.
void ResponseToApp(MonitorJson& value, MonitorJson& msg, uint16_t result)
{
  int taskid = 10;
  if (const auto it = msg.FindMember("taskid"); it != msg.MemberEnd() && it->value.IsInt())
    taskid = it->value.GetInt();
  else
    LOG_ERR << "[ResponseToApp]msg do not have taskid or taskid is not int!";

  value.AddInt("result", result);
  if (taskid < 10)
    value.AddInt("status", getMonitorState());
  else
    value.AddInt("status", getMMIState());
  value.AddInt("df", ++g_responseCounter);

  msg.RemoveMember("value");
  msg.AddString("value", value.ToString());
  MonitorMsgCenter::GetPtr().SendMsg(msg.ToString());
  if (g_responseCounter > 10000)
    g_responseCounter = 0;
}

// 0x47ab6c
void ResponseToApp(const std::string& method, int taskid, const std::string& operType, const std::string& operation,
                   int result, int status, long ossId)
{
  MonitorJson outer;
  MonitorJson value;
  value.AddString("operType", operType);
  value.AddString("operation", operation);
  value.AddInt("result", result);
  value.AddInt("status", status);
  outer.AddString("method", method);
  outer.AddInt("taskid", taskid);
  if (ossId != 0)
    outer.AddInt("ossId", ossId);
  outer.AddString("value", value.ToString());
  MonitorMsgCenter::GetPtr().SendMsg(outer.ToString());
}

// 0x47af44
void ResponseToApp(int taskid, const std::string& operType, const std::string& operation)
{
  MonitorJson outer;
  MonitorJson value;
  value.AddString("operType", operType);
  value.AddString("operation", operation);
  outer.AddInt("taskid", taskid);
  outer.AddString("method", "action");
  outer.AddString("value", value.ToString());
  MonitorMsgCenter::GetPtr().SendMsg(outer.ToString());
}

// 0x47b230
void ResponseOpState(const std::string& what)
{
  int status = 0;
  int taskid = 0;
  if (what == "camera")
  {
    status = getMonitorState();
    taskid = TASK_CAMERA_PROP;
  }
  else if (what == "video")
  {
    status = getMMIState();
    taskid = TASK_VIDEO_PROP;
  }
  else if (what == "monitor")
  {
    status = getMonitorState();
    taskid = TASK_MONITOR;
  }
  ResponseToApp("properties_changed", taskid, "end", what, 0, status, 0);
}

// 0x47b3e8
void ResponseAgoraFail() { ResponseToApp("action", 1, "init", "start", VM_ERR_AGORA_INIT, 0, 0); }

// 0x47b860
MonitorJson JsonForUploadUrl(const std::string& type, const std::string& filename, const std::string& key, int category,
                             int taskid)
{
  MonitorJson j;
  j.AddString("method", "oss_upload_url");
  j.AddString("type", type);
  j.AddString("filename", filename);
  j.AddString("key", key);
  j.AddInt("category", category);
  j.AddInt("taskid", taskid);
  return j;
}

// 0x47bb3c
MonitorJson JsonForResponseUpload(int taskid, long ossId, int result, const std::string& filename,
                                  const std::string& type, const std::string& key, int status, int filesize)
{
  MonitorJson j;
  j.AddString("method", "properties_changed");
  j.AddInt("taskid", taskid);
  j.AddString("value", JsonForValue("recordVideo", "start", result, status).ToString());
  j.AddInt64("ossId", ossId);
  j.AddInt("filesize", filesize);
  j.AddString("filename", filename);
  j.AddInt("category", 0);
  j.AddString("type", type);
  j.AddString("key", key);
  return j;
}

// 0x47c038
MonitorJson JsonForValue(const std::string& operType, const std::string& operation, int result, int status)
{
  MonitorJson j;
  j.AddString("operType", operType);
  j.AddString("operation", operation);
  j.AddInt("result", result);
  j.AddInt("status", status);
  return j;
}

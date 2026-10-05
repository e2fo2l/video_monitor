#include "monitor_agora_service.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include "monitor_audio.h"
#include "monitor_common.h"
#include "monitor_json.h"
#include "ms_log.h"
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"
#include "util/thread_name.h"
#include "vendor/agora_rtc_api.h"
#include "vm_paths.h"

std::atomic<bool> MonitorAgoraService::m_bConnectSuccess{false};
std::atomic<bool> MonitorAgoraService::m_bSendFirst{false};

// Static-init singleton (0x466ef4).
MonitorAgoraService& MonitorAgoraService::GetPtr()
{
  // Never destroyed, like the original, so no destructor runs at exit while the worker
  // threads still use it.
  static auto* instance = new MonitorAgoraService(); // NOLINT(cppcoreguidelines-owning-memory)
  return *instance;
}

MonitorAgoraService::MonitorAgoraService() = default; // 0x464428

// 0x46458c
bool MonitorAgoraService::VerifyLicense(const std::string& path)
{
  std::ifstream in;
  in.open(path);
  if (!in.is_open())
  {
    LOG_ERR << "[MonitorAgoraService]VerifyLicense open " << path << " faild";
    return false;
  }
  const std::string cert{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  in.close();
  const int err = agora_rtc_license_verify(cert.c_str(), static_cast<int>(cert.size()), nullptr, 0);
  if (err != 0)
  {
    LOG_ERR << "[MonitorAgoraService]agora_rtc_license_verify failed,err code is " << err;
    return false;
  }
  return true;
}

// 0x464898
void MonitorAgoraService::EventHandlerinit(agora_rtc_event_handler_t* h)
{
  h->on_join_channel_success = _on_join_channel_success;
  h->on_connection_lost = __on_connection_lost;
  h->on_rejoin_channel_success = __on_rejoin_channel_success;
  h->on_user_joined = __on_user_joined;
  h->on_user_offline = __on_user_offline;
  h->on_user_mute_audio = __on_user_mute_audio;
  h->on_user_mute_video = __on_user_mute_video;
  h->on_target_bitrate_changed = __on_target_bitrate_changed;
  h->on_key_frame_gen_req = __on_key_frame_gen_req;
  h->on_video_data = __on_video_data;
  h->on_error = __on_error;
  h->on_audio_data = __on_audio_data;
}

// 0x46496c — retried every 5 s by main() until it succeeds.
bool MonitorAgoraService::MonitorAgoraInit()
{
  m_channel = "";
  m_bExit = false;
  m_bConnectSuccess = false;
  m_bSendFirst = false;
  m_areaCodes = {{"cn", 1}, {"eu", 4}, {"us", 2}, {"sg", 8}, {"ru", 4}};

  if (!VerifyLicense(vmpath::AGORA_CERTIFICATE))
  {
    LOG_ERR << "[MonitorAgoraService]VerifyLicense failed";
    return false;
  }
  unsigned areaCode = 0;
  if (!GetAreaCode(areaCode))
  {
    LOG_ERR << "[MonitorAgoraService]GetAreaCode failed";
    return false;
  }

  rtc_service_option_t opt{};
  opt.area_code = areaCode;
  LOG_ERR << "[MonitorAgoraService] AgroaAreaCode is " << opt.area_code;
  opt.storage_dir = vmpath::AGORA_STORAGE_DIR;
  EventHandlerinit(&m_eventHandler);

  const int err = agora_rtc_init(GetSysConf().appid.c_str(), &m_eventHandler, &opt);
  if (err != 0)
  {
    LOG_ERR << "[MonitorAgoraService]agora_rtc_init failed with " << agora_rtc_err_2_str(err);
    return false;
  }
  agora_rtc_set_log_level(4);
  if (const int lerr = agora_rtc_config_log(0xc800, 4); lerr != 0)
    LOG_ERR << "[MonitorAgoraService]agora_rtc_config_log failed with " << agora_rtc_err_2_str(lerr);

  std::thread(&MonitorAgoraService::SendVideoToAgoraThread, this).detach();
  return true;
}

// 0x4650f8
int MonitorAgoraService::MonitorAgoraUinit()
{
  m_bExit = true;
  agora_rtc_fini();
  return 0;
}

// 0x465120
bool MonitorAgoraService::MonitorAgoraConnect(const std::string& token, const std::string& channelId,
                                              const std::string& encryptionKey)
{
  m_bConnectSuccess = false;
  m_bSendFirst = false;

  rtc_channel_options_t opt{};
  opt.auto_subscribe_audio = true;
  opt.auto_subscribe_video = false;
  opt.audio_codec_opt.audio_codec_type = 1;
  opt.audio_codec_opt.pcm_sample_rate = 16000;
  opt.audio_codec_opt.pcm_channel_num = 1;
  m_channel = channelId;
  opt.enable_aut_encryption = true;

  const std::string params = GetEncryptionMsg(true, "SM4-128-ECB", encryptionKey);
  int err = agora_rtc_set_params(params.c_str());
  if (err != 0)
  {
    LOG_ERR << "[MonitorAgoraService]agora_rtc_set_params EncryptionMsg failed with " << agora_rtc_err_2_str(err);
    return false;
  }

  const std::string uid = std::to_string(GetDeviceDid());
  err = agora_rtc_join_channel(m_channel.c_str(), uid.c_str(), token.c_str(), token.size(), &opt);
  if (err != 0)
  {
    LOG_ERR << "[MonitorAgoraService]MonitorAgoraConnect failed with " << agora_rtc_err_2_str(err);
    return false;
  }
  if (!WaitJoinChannel(30))
  {
    LOG_ERR << "[MonitorAgoraService]MonitorAgoraConnect failed";
    return false;
  }
  return true;
}

// 0x4655c8
bool MonitorAgoraService::MonitorAgoraDisConnect()
{
  m_bConnectSuccess = false;
  m_bSendFirst = false;
  const int err = agora_rtc_leave_channel(m_channel.c_str());
  if (err != 0)
  {
    LOG_ERR << "[MonitorAgoraService]MonitorAgoraDisConnect failed with " << agora_rtc_err_2_str(err);
    return false;
  }
  return true;
}

// 0x465718 — {"rtc.encryption":{"enable":true,"mode":"SM4-128-ECB","master_key":"..."}}
std::string MonitorAgoraService::GetEncryptionMsg(bool enable, const std::string& mode, const std::string& key)
{
  MonitorJson j;
  auto& a = j.GetAllocator();
  rapidjson::Value enc(rapidjson::kObjectType);
  enc.AddMember("enable", rapidjson::Value().SetBool(enable), a);
  enc.AddMember("mode", rapidjson::Value().SetString(mode.data(), static_cast<rapidjson::SizeType>(mode.size()), a), a);
  enc.AddMember("master_key", rapidjson::Value().SetString(key.data(), static_cast<rapidjson::SizeType>(key.size()), a),
                a);
  j.AddMember("rtc.encryption", enc, a);
  return j.ToString();
}

// 0x465904 — drains the H.264 queue filled by the TRecorder callback into Agora.
void MonitorAgoraService::SendVideoToAgoraThread()
{
  SetThreadName("SendVideoToAgoraThread");
  // NOLINTBEGIN(cppcoreguidelines-owning-memory): buf.data is malloc'd by the TRecorder callback
  // and handed over through the queue; this consumer frees it.
  while (!m_bExit)
  {
    usleep(10000);
    H264Buffer buf{};
    if (!getQueueH264Buffer(buf))
      continue;
    if (!m_bConnectSuccess)
    {
      free(buf.data);
      continue;
    }
    if (m_bSendFirst)
    {
      if (!buf.isKeyFrame)
      { // the first frame after joining must be an IDR
        free(buf.data);
        continue;
      }
      m_bSendFirst = false;
    }
    video_frame_info_t info{.data_type = VIDEO_DATA_TYPE_H264,
                            .frame_type = VIDEO_FRAME_AUTO_DETECT,
                            .frame_rate = VIDEO_FRAME_RATE_FPS_15};
    const int err = agora_rtc_send_video_data(m_channel.c_str(), 0, buf.data, static_cast<size_t>(buf.size), &info);
    if (err < 0)
      LOG_ERR << "[MonitorAgoraService]agora_rtc_send_video_data failed " << err << " with "
              << agora_rtc_err_2_str(err);
    SaveH264(buf);
    free(buf.data);
  }
  // NOLINTEND(cppcoreguidelines-owning-memory)
}

// 0x465b84
bool MonitorAgoraService::WaitJoinChannel(unsigned long seconds)
{
  const long start = GetCpuTime();
  for (;;)
  {
    usleep(5000);
    if (m_bConnectSuccess)
      return true;
    if (static_cast<unsigned long>(GetCpuTime() - start) >= seconds * 1000000)
      return m_bConnectSuccess;
  }
}

// 0x465c08
bool MonitorAgoraService::getQueueH264Buffer(H264Buffer& out)
{
  std::scoped_lock const lock(m_h264Mutex);
  if (m_h264Queue.empty())
    return false;
  out = m_h264Queue.front();
  m_h264Queue.pop();
  return true;
}

// 0x465ca8 — NOTE(orig): unbounded; frames pile up in RAM if the sender thread stalls.
void MonitorAgoraService::setQueueH264Buffer(const H264Buffer& buf)
{
  std::scoped_lock const lock(m_h264Mutex);
  m_h264Queue.push(buf);
  ++m_streamCount;
  if (m_streamCount == 150 || m_streamCount == 2700 || m_streamCount == 1)
    LOG_ERR << "[setQueueH264Buffer] Got " << m_streamCount << " streams";
}

// 0x465e40
void MonitorAgoraService::SetStreamCount(long count)
{
  std::scoped_lock const lock(m_h264Mutex);
  m_streamCount = count;
}

// 0x465e80
void MonitorAgoraService::_on_join_channel_success(const char* channel, int /*unused*/)
{
  m_bConnectSuccess = true;
  m_bSendFirst = true;
  if (channel)
    LOG_ERR << "[MonitorAgoraService]_on_join_channel_success " << channel;
}

// 0x465fb4
void MonitorAgoraService::__on_rejoin_channel_success(const char* channel, int /*unused*/)
{
  m_bConnectSuccess = true;
  m_bSendFirst = true;
  if (channel)
    LOG_ERR << "[MonitorAgoraService]__on_rejoin_channel_success " << channel;
}

// 0x4660e8
void MonitorAgoraService::__on_connection_lost(const char* channel)
{
  m_bConnectSuccess = false;
  m_bSendFirst = false;
  if (channel)
    LOG_ERR << "[MonitorAgoraService]__on_connection_lost " << channel;
}

// 0x4662e4 — codes 0, 122 and 200 are informational.
void MonitorAgoraService::__on_error(const char* channel, int code, const char* msg)
{
  LOG_ERR << "[MonitorAgoraService]agora on error " << (msg ? msg : "") << "error code is" << code;
  if (code != 0 && code != 122 && code != 200)
  {
    m_bConnectSuccess = false;
    m_bSendFirst = false;
  }
  (void)channel;
}

// 0x46656c — intercom audio from the phone.
void MonitorAgoraService::__on_audio_data(const char* /*unused*/, uint32_t /*unused*/, uint16_t /*unused*/,
                                          audio_data_type_e /*unused*/, const void* data, size_t len)
{
  if (!MonitorAudio::GetPtr().IsAudioOpen())
    return;
  if (len == 0 || data == nullptr)
  {
    LOG_INFO << "[__on_audio_data] RCV empty audio Data!!";
    return;
  }
  AudioDataFromAgora a;
  a.size = len;
  // Queued as a raw buffer; freed by the consumer (MonitorAudio) or when the queue overflows.
  a.audioBuffer = malloc(len); // NOLINT(cppcoreguidelines-owning-memory)
  memcpy(a.audioBuffer, data, len);
  GetPtr().AddQueueAudio(a);
}

// 0x4666ec
bool MonitorAgoraService::GetQueueAudio(AudioDataFromAgora& out)
{
  std::scoped_lock const lock(m_audioMutex);
  if (m_audioQueue.empty())
    return false;
  out = m_audioQueue.front();
  m_audioQueue.pop();
  return true;
}

// 0x466770
size_t MonitorAgoraService::GetQueueAudioSize()
{
  std::scoped_lock const lock(m_audioMutex);
  return m_audioQueue.size();
}

// 0x4667b8 — keeps at most 40 packets.
void MonitorAgoraService::AddQueueAudio(const AudioDataFromAgora& data)
{
  SaveAudio(data);
  std::scoped_lock const lock(m_audioMutex);
  m_audioQueue.push(data);
  if (m_audioQueue.size() > 40)
  {
    free(m_audioQueue.front().audioBuffer); // NOLINT(cppcoreguidelines-owning-memory): queued raw buffer
    m_audioQueue.pop();
  }
}

// 0x466884
void MonitorAgoraService::CleanQueueAudio()
{
  std::scoped_lock const lock(m_audioMutex);
  while (!m_audioQueue.empty())
  {
    free(m_audioQueue.front().audioBuffer); // NOLINT(cppcoreguidelines-owning-memory): queued raw buffer
    m_audioQueue.pop();
  }
}

// 0x466958
bool MonitorAgoraService::GetAreaCode(unsigned& areaCode)
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
  std::string area;
  if (!ReadFile(regionFile, area))
    return false;
  if (area.empty())
  {
    LOG_ERR << "[GetAreaCode] open " << regionFile << " value is empty";
    return false;
  }
  if (flag == "miiot" && area == "\n")
  {
    areaCode = 1; // cn
    return true;
  }
  // NOTE(orig): the file content is not trimmed, so "eu\n" misses the table and the SDK gets
  // 0xFFFFFFFF (global).
  const auto it = m_areaCodes.find(area);
  areaCode = (it != m_areaCodes.end()) ? it->second : 0xffffffffU;
  LOG_ERR << "[GetAreaCode] Area is " << area << " AgroaAreaCode is " << areaCode;
  return true;
}

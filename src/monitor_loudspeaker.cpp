// monitor_loudspeaker.cpp + monitor_microphone.cpp
#include "monitor_loudspeaker.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <rapidjson/filereadstream.h>
#include <string>
#include <sys/types.h>
#include <utility>

#include "monitor_common.h"
#include "monitor_json.h"
#include "ms_log.h"
#include "rapidjson/reader.h"
#include "vm_paths.h"

unsigned PERIODIC_TIME = 48000;

namespace
{

// Shared by both ReadConf variants (0x486028 / 0x48adcc).
struct PcmConf
{
  unsigned samplerate = 0, channelnum = 0;
  int pcmframes = 0, pcmformat = 0;
  std::string servicename;
};

bool readPcmConf(const std::string& path, const char* tag, PcmConf& c)
{
  // NOLINTBEGIN(cppcoreguidelines-owning-memory): rapidjson's FileReadStream reads from a C stdio
  // handle, closed right below.
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp)
  {
    LOG_ERR << tag << "open conf failed :" << path;
    return false;
  }
  char buf[4096];
  rapidjson::FileReadStream is(fp, buf, sizeof buf);
  fclose(fp);
  // NOLINTEND(cppcoreguidelines-owning-memory)
  MonitorJson doc;
  doc.ParseStream<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(is);
  if (doc.HasParseError())
  {
    LOG_ERR << tag << "config json parse err!";
    return false;
  }
  if (!doc.IsObject())
  {
    LOG_ERR << tag << "config format err!";
    return false;
  }
  return GetJsonParam(doc, "samplerate", tag, c.samplerate) && GetJsonParam(doc, "channelnum", tag, c.channelnum) &&
         GetJsonParam(doc, "pcmframes", tag, c.pcmframes) && GetJsonParam(doc, "pcmformat", tag, c.pcmformat) &&
         GetJsonParam(doc, "servicename", tag, c.servicename);
}

// NOTE(orig): bit depth -> snd_pcm_format_t, but 8/24/32 map to the *unsigned* formats.
int mapPcmFormat(int bits)
{
  switch (bits)
  {
  case 8:
    return SND_PCM_FORMAT_U8; // 1
  case 16:
    return SND_PCM_FORMAT_S16_LE; // 2
  case 24:
    return SND_PCM_FORMAT_U24_LE; // 8
  case 32:
    return SND_PCM_FORMAT_U32_LE; // 12
  default:
    return -1;
  }
}

// The hw_params blob is snd_pcm_hw_params_alloca()'d in the original and the pointer is
// stored in the object, i.e. it dangles after Init returns. We allocate it properly.
bool configurePcm(snd_pcm_t* pcm, snd_pcm_hw_params_t*& params, int format, unsigned channels, unsigned& rate,
                  snd_pcm_uframes_t& period, const char* tag)
{
  if (!params)
    snd_pcm_hw_params_malloc(&params);
  int dir = 0;
  if (const int err = snd_pcm_hw_params_any(pcm, params); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_any failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_set_access failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params_set_format(pcm, params, static_cast<snd_pcm_format_t>(format)); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_set_format failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params_set_channels(pcm, params, channels); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_set_channels failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, &dir); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_set_rate_near failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params_set_period_size_near(pcm, params, &period, &dir); err < 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_set_period_size_near failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params(pcm, params); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params_get_period_size(params, &period, &dir); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_get_period_size failed " << snd_strerror(err);
    return false;
  }
  if (const int err = snd_pcm_hw_params_get_period_time(params, &PERIODIC_TIME, &dir); err != 0)
  {
    LOG_ERR << tag << "snd_pcm_hw_params_get_period_time failed " << snd_strerror(err);
    return false;
  }
  return true;
}

bool closePcm(snd_pcm_t*& pcm, const char* tag)
{
  if (!pcm)
  {
    LOG_ERR << tag << "m_pPcmHandle is nullptr  ";
    return false;
  }
  int err = snd_pcm_drain(pcm);
  if (err != 0)
  {
    LOG_ERR << tag << "snd_pcm_drain failed " << snd_strerror(err);
    return false;
  }
  err = snd_pcm_close(pcm);
  pcm = nullptr;
  if (err != 0)
  {
    LOG_ERR << tag << "snd_pcm_close failed " << snd_strerror(err);
    return false;
  }
  return true;
}
} // namespace

// ---- MonitorLoudspeaker -------------------------------------------------------------------

// 0x486028
bool MonitorLoudspeaker::ReadConf(const std::string& path)
{
  PcmConf c;
  if (!readPcmConf(path, "[Loudspeaker]", c))
    return false;
  m_sampleRate = c.samplerate;
  m_channels = c.channelnum;
  m_serviceName = c.servicename;
  m_periodFrames = static_cast<snd_pcm_uframes_t>(c.pcmframes);
  m_format = mapPcmFormat(c.pcmformat);
  if (m_format < 0)
  {
    LOG_ERR << "[Loudspeaker]pcmformat val wrong:" << c.pcmformat;
    return false;
  }
  return true;
}

// 0x484768 — the config is parsed once per process.
bool MonitorLoudspeaker::LoudspeakerInit()
{
  static bool confLoaded = false; // DAT_0053f809
  if (!confLoaded)
  {
    if (!ReadConf(vmpath::LOUDSPEAKER_CFG))
    {
      LOG_ERR << "[LoudspeakerInit]read conf failed: " << vmpath::LOUDSPEAKER_CFG;
      return false;
    }
    confLoaded = true;
  }
  const int err = snd_pcm_open(&m_pPcmHandle, m_serviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err != 0)
  {
    LOG_ERR << "[LoudspeakerInit]snd_pcm_open failed " << snd_strerror(err);
    return false;
  }
  return configurePcm(m_pPcmHandle, m_params, m_format, m_channels, m_sampleRate, m_periodFrames, "[LoudspeakerInit]");
}

// 0x485514
bool MonitorLoudspeaker::LoudspeakerUninit() { return closePcm(m_pPcmHandle, "[LoudspeakerUninit]"); }

// 0x485844 — probes whether another process has released the speaker.
bool MonitorLoudspeaker::TryControlLoudSpeaker()
{
  if (snd_pcm_open(&m_pPcmHandle, m_serviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0) != 0)
    return false;
  const int err = snd_pcm_close(m_pPcmHandle);
  m_pPcmHandle = nullptr;
  if (err != 0)
    LOG_ERR << "[TryControlLoudSpeaker]snd_pcm_close failed " << snd_strerror(err);
  return true;
}

// 0x4859b8
bool MonitorLoudspeaker::LoudspeakerWriteData(const AudioDataFromAgora& data)
{
  if (!data.audioBuffer)
  {
    LOG_ERR << "[LoudspeakerWriteData]data.audioBuffer is nullptr";
    return false;
  }
  const snd_pcm_sframes_t frames = snd_pcm_bytes_to_frames(m_pPcmHandle, static_cast<ssize_t>(data.size));
  snd_pcm_sframes_t const written =
      snd_pcm_writei(m_pPcmHandle, data.audioBuffer, static_cast<snd_pcm_uframes_t>(frames));
  if (written == -EPIPE)
  {
    LOG_ERR << "[LoudspeakerWriteData]snd_pcm_writei failed " << snd_strerror(static_cast<int>(written));
    if (const int err = snd_pcm_prepare(m_pPcmHandle); err != 0)
      LOG_ERR << "[LoudspeakerWriteData]snd_pcm_prepare failed " << snd_strerror(err);
    return false;
  }
  if (written < 0)
  {
    LOG_ERR << "[LoudspeakerWriteData]failed " << snd_strerror(static_cast<int>(written));
    if (const int err = snd_pcm_recover(m_pPcmHandle, static_cast<int>(written), 0); err != 0)
      LOG_ERR << "[LoudspeakerWriteData]snd_pcm_recover failed " << snd_strerror(err);
    return false;
  }
  if (written != frames)
  {
    LOG_ERR << "[LoudspeakerWriteData]frames not match failed " << written << " vs " << frames;
    return false;
  }
  return true;
}

// ---- MonitorMicrophone (dead code in the shipped binary) ----------------------------------

// 0x48adcc
bool MonitorMicrophone::ReadConf(const std::string& path)
{
  PcmConf c;
  if (!readPcmConf(path, "[Mic]", c))
    return false;
  m_sampleRate = c.samplerate;
  m_channels = c.channelnum;
  m_serviceName = c.servicename;
  m_periodFrames = static_cast<snd_pcm_uframes_t>(c.pcmframes);
  m_format = mapPcmFormat(c.pcmformat);
  if (m_format < 0)
  {
    LOG_ERR << "[Mic]pcmformat val wrong:" << c.pcmformat;
    return false;
  }
  return true;
}

// 0x489970
bool MonitorMicrophone::MicInit()
{
  if (!ReadConf(vmpath::MICROPHONE_CFG))
  {
    LOG_ERR << "[MicInit]read config failed";
    return false;
  }
  const int err = snd_pcm_open(&m_pPcmHandle, m_serviceName.c_str(), SND_PCM_STREAM_CAPTURE, 0);
  if (err != 0)
  {
    LOG_ERR << "[MicInit]snd_pcm_open failed " << snd_strerror(err);
    return false;
  }
  if (!configurePcm(m_pPcmHandle, m_params, m_format, m_channels, m_sampleRate, m_periodFrames, "[MicInit]"))
    return false;
  m_bufferBytes = m_periodFrames * m_channels * 2;
  return true;
}

// 0x48a6f4
bool MonitorMicrophone::MicUninit() { return closePcm(m_pPcmHandle, "[MicUninit]"); }

// 0x48aa28
bool MonitorMicrophone::MicReadAudioData(AudioData& out)
{
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): handed to the caller as a raw buffer, which frees it
  out.buffer = malloc(m_bufferBytes);
  out.size = static_cast<int>(m_bufferBytes);
  const snd_pcm_sframes_t n = snd_pcm_readi(m_pPcmHandle, out.buffer, m_periodFrames);
  if (n == -EPIPE)
  {
    if (const int err = snd_pcm_prepare(m_pPcmHandle); err != 0)
      LOG_ERR << "[MicReadAudioData]snd_pcm_prepare failed " << snd_strerror(err);
    return false;
  }
  if (n < 0)
  {
    LOG_ERR << "[MicReadAudioData]snd_pcm_readi failed " << snd_strerror(static_cast<int>(n));
    return false;
  }
  if (std::cmp_not_equal(m_periodFrames, n))
  {
    LOG_ERR << "[MicReadAudioData]snd_pcm_readi failed" << n;
    return false;
  }
  return true;
}

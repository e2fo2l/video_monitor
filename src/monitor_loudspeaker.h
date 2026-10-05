// ALSA playback (intercom audio from the app) and capture (unused) wrappers.
#pragma once

#include <string>

#include <alsa/asoundlib.h>

#include "monitor_common.h"

struct AudioData
{
  void* buffer = nullptr;
  int size = 0;
};

class MonitorLoudspeaker
{
public:
  MonitorLoudspeaker() = default;
  bool LoudspeakerInit();
  bool LoudspeakerUninit();
  bool TryControlLoudSpeaker();
  bool LoudspeakerWriteData(const AudioDataFromAgora& data);

private:
  bool ReadConf(const std::string& path);

  unsigned m_sampleRate = 0;               // +0x00 "samplerate"
  unsigned m_channels = 0;                 // +0x04 "channelnum"
  std::string m_serviceName;               // +0x08 "servicename" (ALSA device)
  int m_format = 0;                        // +0x28 "pcmformat" mapped to snd_pcm_format_t
  snd_pcm_t* m_pPcmHandle = nullptr;       // +0x30
  snd_pcm_hw_params_t* m_params = nullptr; // +0x38
  snd_pcm_uframes_t m_periodFrames = 0;    // +0x40 "pcmframes"
};

// Linked in but never used by the shipped binary.
class MonitorMicrophone
{
public:
  MonitorMicrophone() = default;
  bool MicInit();
  bool MicUninit();
  bool MicReadAudioData(AudioData& out);

private:
  bool ReadConf(const std::string& path);

  unsigned m_sampleRate = 0;               // +0x00
  unsigned m_channels = 0;                 // +0x04
  size_t m_bufferBytes = 0;                // +0x08
  std::string m_serviceName;               // +0x10
  int m_format = 0;                        // +0x30
  snd_pcm_t* m_pPcmHandle = nullptr;       // +0x38
  snd_pcm_hw_params_t* m_params = nullptr; // +0x40
  snd_pcm_uframes_t m_periodFrames = 0;    // +0x48
};

extern unsigned PERIODIC_TIME;

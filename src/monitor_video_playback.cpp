// Playback of a recorded clip through the Agora channel.
#include "monitor_common.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <string>
#include <unistd.h>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "monitor_agora_service.h"
#include "monitor_video.h"
#include "ms_log.h"
#include "util/thread_name.h"
#if VM_COMPAT
#include "util/h264_param_sets.h"
#endif

namespace
{
const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

void freePacket(AVPacket* pkt)
{
#if LIBAVFORMAT_VERSION_MAJOR < 59
  av_free_packet(pkt);
#else
  av_packet_unref(pkt);
#endif
}
} // namespace

// 0x4a1d38 — NOTE(orig): returns the last index of the leading run of video streams, or -1.
int GetVideoFrameEnd(AVFormatContext* ctx)
{
  int idx = -1;
  for (unsigned i = 0; i < ctx->nb_streams && ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO; ++i)
    idx = static_cast<int>(i);
  return idx;
}

// 0x4a1db0 — converts one MP4 (length-prefixed) sample to Annex-B and queues it.
// NOTE(orig): only the first NAL's 4-byte length is replaced, so multi-NAL samples break.
int en_queue(AVStream* st, AVPacket* pkt)
{
  pkt->stream_index = 0;
  const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
  const int fps = (st->avg_frame_rate.den != 0) ? st->avg_frame_rate.num / st->avg_frame_rate.den : 0;
  (void)fps; // computed but unused in the original

  H264Buffer buf;
  buf.isKeyFrame = key;
  buf.size = pkt->size;
  if (key)
  {
#if VM_COMPAT
    // NOTE(orig): a 720p SPS/PPS was inlined here regardless of the configured size.
    static const std::vector<uint8_t> params = BuildSpsPps(1280, 720);
#else
    const std::vector<uint8_t>& params = GetSpsPps(); // configured encoder size
#endif
    const size_t hdr = params.size() + sizeof kStartCode;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): queued raw buffer, freed by SendVideoToAgoraThread
    buf.data = static_cast<uint8_t*>(malloc(hdr + static_cast<size_t>(pkt->size) - 4));
    memcpy(buf.data, params.data(), params.size());
    memcpy(buf.data + params.size(), kStartCode, sizeof kStartCode);
    memcpy(buf.data + hdr, pkt->data + 4, static_cast<size_t>(pkt->size - 4));
    buf.size = static_cast<int>(hdr) + pkt->size - 4;
  }
  else
  {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): queued raw buffer, freed by SendVideoToAgoraThread
    buf.data = static_cast<uint8_t*>(malloc(static_cast<size_t>(pkt->size)));
    memcpy(buf.data, kStartCode, sizeof kStartCode);
    memcpy(buf.data + 4, pkt->data + 4, static_cast<size_t>(pkt->size - 4));
  }
  MonitorAgoraService::GetPtr().setQueueH264Buffer(buf);
  freePacket(pkt);
  return 0;
}

// 0x4a1f48
void MonitorVideo::VideoPlaybackStart(const std::string& name)
{
  SetThreadName("video_playback");
  if (m_token.empty() || m_channelId.empty() || m_encryptionKey.empty())
  {
    LOG_ERR << "[palyback]param empty token:" << m_token << " channelid:" << m_channelId << "encryptionKey";
    VideoPlaybackEndClean(nullptr, false, "", false);
    return;
  }
  if (!MonitorAgoraService::GetPtr().MonitorAgoraConnect(m_token, m_channelId, m_encryptionKey))
  {
    LOG_ERR << "[palyback]agora connect fail!";
    VideoPlaybackEndClean(nullptr, true, "", false);
    return;
  }
  const std::string path = GetSysConf().video_filepath + name;
  if (!DecryptFile(path))
  {
    LOG_ERR << "[palyback]decrypt fail!";
    VideoPlaybackEndClean(nullptr, true, "", false);
    return;
  }
#if LIBAVFORMAT_VERSION_MAJOR < 58 || (LIBAVFORMAT_VERSION_MAJOR == 58 && LIBAVFORMAT_VERSION_MINOR < 9)
  av_register_all();
#endif
  AVFormatContext* ctx = nullptr;
  if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
  {
    LOG_ERR << "[palyback]could not open input file";
    VideoPlaybackEndClean(ctx, true, path, false);
    return;
  }
  if (avformat_find_stream_info(ctx, nullptr) < 0)
  {
    LOG_ERR << "[palyback]Failed to retrieve input stream infomation";
    VideoPlaybackEndClean(ctx, true, path, false);
    return;
  }
  const int videoIdx = GetVideoFrameEnd(ctx);
  if (videoIdx < 0)
  {
    LOG_ERR << "[palyback]no video frame";
    VideoPlaybackEndClean(ctx, true, path, false);
    return;
  }
  AVPacket* pkt = av_packet_alloc();
  while (av_read_frame(ctx, pkt) >= 0)
  {
    if (pkt->stream_index != videoIdx)
    {
      av_packet_unref(pkt); // NOTE(orig): leaked
      continue;
    }
    en_queue(ctx->streams[pkt->stream_index], pkt);
    if (!m_bPlayback)
      break;
    usleep(30000);
  }
  av_packet_free(&pkt);
  VideoPlaybackEndClean(ctx, true, path, true);
}

// 0x4a2978 — re-encrypts the clip and returns to idle.
void MonitorVideo::VideoPlaybackEndClean(AVFormatContext* ctx, bool disconnect, const std::string& path, bool ok)
{
  m_bPlayback = false;
  const std::string key; // NOTE(orig): empty key -> AES key is md5("")
  if (disconnect)
    MonitorAgoraService::GetPtr().MonitorAgoraDisConnect();
  // NOTE(orig): inverted check (`if (ctx == nullptr) avformat_close_input(&ctx)`), so the
  // context always leaked. Fixed here.
  if (ctx)
    avformat_close_input(&ctx);
  if (!path.empty() && !EncryptFile(path, key))
    LOG_ERR << "[playback]end clean encrypt fail:" << path;
  setMonitorState(MONITOR_SYS_BUTT);
  if (ok)
    LOG_INFO << "[playback]==============RIGHT END=============";
  else
    LOG_ERR << "[playback]==============ERROR END=============";
}

// 0x4a2cbc
void MonitorVideo::VideoPlaybackStop() { m_bPlayback = false; }

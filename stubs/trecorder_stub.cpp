// Host stand-in for libtrecorder.so.
//   VM_STUB_H264=<file.h264>  Annex-B stream replayed (looped, ~15 fps) through the frame callback.
// Access units are split at slice NALs; SPS/PPS/SEI stay glued to the following slice, which
// mimics an encoder that emits parameter sets in band. Set VM_STUB_STRIP_SPS=1 to drop them and
// mimic the stock encoder (IDR-only key frames, which triggers video_monitor's SPS injection).
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <vendor/trecorder.h>

namespace
{
struct Recorder
{
  TRecorderCallback cb = nullptr;
  void* user = nullptr;
  std::atomic<bool> running{false};
  std::thread worker;
};

// Splits on 3- and 4-byte start codes; every NAL is re-emitted with a 4-byte start code, the
// way the Allwinner encoder delivers them.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): every index is bounded
// by the loop conditions (i + 2 < size, k < payload.size(), e > b).
std::vector<std::vector<uint8_t>> splitAccessUnits(const std::vector<uint8_t>& s, bool stripSps)
{
  std::vector<size_t> payload; // index of the first byte after each start code
  for (size_t i = 0; i + 2 < s.size(); ++i)
  {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
    {
      payload.push_back(i + 3);
      i += 2;
    }
  }
  std::vector<std::vector<uint8_t>> aus;
  std::vector<uint8_t> cur;
  for (size_t k = 0; k < payload.size(); ++k)
  {
    const size_t b = payload[k];
    size_t e = (k + 1 < payload.size()) ? payload[k + 1] - 3 : s.size();
    while (e > b && s[e - 1] == 0) // trailing zero of a following 4-byte start code
      --e;
    const int type = s[b] & 0x1f;
    if (stripSps && (type == 7 || type == 8))
      continue;
    cur.insert(cur.end(), {0, 0, 0, 1});
    cur.insert(cur.end(), s.begin() + static_cast<long>(b), s.begin() + static_cast<long>(e));
    if (type == 1 || type == 5)
    {
      aus.push_back(std::move(cur));
      cur.clear();
    }
  }
  return aus;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

void run(Recorder* r)
{
  // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the process modifies the environment
  const char* path = std::getenv("VM_STUB_H264");
  if (!path)
  {
    std::cerr << "[trecorder-stub] VM_STUB_H264 not set, no frames will be produced\n";
    return;
  }
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the process modifies the environment
  auto aus = splitAccessUnits(data, std::getenv("VM_STUB_STRIP_SPS") !=
                                        nullptr); // non-const: frames are handed out as uint8_t*
  std::cerr << "[trecorder-stub] " << aus.size() << " access units from " << path << '\n';
  while (r->running && !aus.empty())
  {
    for (auto& au : aus)
    {
      if (!r->running)
        break;
      TRecorderVideoFrame f{};
      f.data = au.data();
      f.size = static_cast<int32_t>(au.size());
      // Every access unit starts with a 4-byte start code and a NAL header byte.
      const int nalType = au[4] & 0x1f; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      f.is_key_frame = (nalType == 7 || nalType == 5) ? 1 : 0;
      r->cb(r->user, T_RECORD_STREAM_FRAME, &f);
      usleep(66000);
    }
  }
}
} // namespace

extern "C"
{

  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): handed out as an opaque C handle; TRrelease deletes it
  TrecoderHandle CreateTRecorder(void) { return new Recorder(); }
  int TRreset(TrecoderHandle /*unused*/) { return 0; }
  int TRSetRecorderCfgPath(const char* /*unused*/, int /*unused*/) { return 0; }
  int TRsetCamera(TrecoderHandle /*unused*/, int /*unused*/) { return 0; }
  int TRsetOutput(TrecoderHandle /*unused*/, char* /*unused*/) { return 0; }
  int TRchangeOutputPath(TrecoderHandle /*unused*/, char* /*unused*/) { return 0; }
  int TRsetMaxRecordTimeMs(TrecoderHandle /*unused*/, int /*unused*/) { return 0; }

  int TRsetRecorderCallback(TrecoderHandle h, TRecorderCallback cb, void* user)
  {
    auto* r = static_cast<Recorder*>(h);
    r->cb = cb;
    r->user = user;
    return 0;
  }

  int TRprepare(TrecoderHandle /*unused*/) { return 0; }

  int TRstart(TrecoderHandle h, int /*unused*/)
  {
    auto* r = static_cast<Recorder*>(h);
    r->running = true;
    r->worker = std::thread(run, r);
    return 0;
  }

  int TRstop(TrecoderHandle h, int /*unused*/)
  {
    auto* r = static_cast<Recorder*>(h);
    r->running = false;
    if (r->worker.joinable())
      r->worker.join();
    return 0;
  }

  int TRrelease(TrecoderHandle h)
  {
    TRstop(h, 0);
    delete static_cast<Recorder*>(h); // NOLINT(cppcoreguidelines-owning-memory): see CreateTRecorder
    return 0;
  }

  int TRmux(TrecoderHandle /*unused*/, int /*unused*/) { return 0; }

} // extern "C"

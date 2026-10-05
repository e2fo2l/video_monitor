// Miscellaneous helpers from the first object file of the original link (G.711 A-law codec,
// send pacing, timing statistics). None of them are referenced by the shipped binary except
// getCurrentSystemTimeChrono (inlined into CallbackFromTRecorder in this reconstruction).
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

#include "monitor_common.h"

namespace
{
struct PacerInfo
{
  int sentCount{};                                 // +0x00
  int intervalMs{};                                // +0x04
  std::chrono::steady_clock::time_point startTime; // +0x08
};

// 0x45a56c — sleep until `count * interval` ms have elapsed since start.
[[maybe_unused]] int waitBeforeNextSend(PacerInfo& p)
{
  const auto now = std::chrono::steady_clock::now();
  ++p.sentCount;
  const long target = static_cast<long>(p.sentCount) * p.intervalMs;
  const long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.startTime).count();
  int wait = static_cast<int>(target - elapsed);
  if (wait > 0)
    wait = usleep(static_cast<useconds_t>(wait) * 1000);
  return wait;
}

// 0x45a784
[[maybe_unused]] long now_ms_t()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 0x45a7bc — prints avg/max/min of (end - begin) every `every` calls.
[[maybe_unused]] void spendTimeInfoStatistics(unsigned long begin, unsigned long end, int every)
{
  static int total = 0;
  static int maxv = 0;
  static int minv = -1;
  static int calls = 0;
  const int d = static_cast<int>(end) - static_cast<int>(begin);
  total += d;
  maxv = std::max(maxv, d);
  if (minv == -1 || d < minv)
    minv = d;
  const int q = (every != 0) ? calls / every : 0;
  if (calls == q * every)
  {
    std::cout << "the averge spend time per " << every << " times is: " << std::fixed << std::setprecision(6)
              << static_cast<double>(total) / every << "\n ";
    std::cout << "the max spend time is:" << maxv << ". the min spend time is:" << minv << '\n';
    maxv = minv = total = calls = 0;
  }
  ++calls;
}

// ---- ITU-T G.711 A-law (0x45ac10..0x45b008) ----------------------------------------------

const short seg_aend[8] = {0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff};
unsigned char g_pcm2alaw[65536];
short g_alaw2pcm[256];

short search(short val, const short* table, short size)
{ // 0x45ac10
  for (short i = 0; i < size; ++i)
    if (val <= table[i])
      return i;
  return size;
}

// 0x45ac88
unsigned char linear2alaw(short pcm)
{
  auto v = static_cast<short>(pcm >> 3);
  unsigned char mask = 0;
  if (v >= 0)
  {
    mask = 0xd5;
  }
  else
  {
    mask = 0x55;
    v = static_cast<short>(~v);
  }
  const short seg = search(v, seg_aend, 8);
  if (seg >= 8)
    return static_cast<unsigned char>(0x7f ^ mask);
  auto aval = static_cast<unsigned char>(seg << 4);
  aval |= (seg < 2) ? ((v >> 1) & 0xf) : ((v >> seg) & 0xf);
  return static_cast<unsigned char>(aval ^ mask);
}

// 0x45ad98
short alaw2linear(unsigned char a)
{
  a ^= 0x55;
  auto t = static_cast<short>((a & 0xf) << 4);
  const int seg = (a & 0x70) >> 4;
  if (seg == 0)
    t += 8;
  else if (seg == 1)
    t += 0x108;
  else
    t = static_cast<short>((t + 0x108) << (seg - 1));
  return ((a & 0x80) != 0) ? t : static_cast<short>(-t);
}

void pcm16_alaw_tableinit()
{ // 0x45ae6c
  // i < table size
  for (int i = 0; i < 65536; ++i)
    g_pcm2alaw[i] = linear2alaw(static_cast<short>(i)); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
}

void alaw_pcm16_tableinit()
{ // 0x45aec4
  // i < table size
  for (int i = 0; i < 256; ++i)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    g_alaw2pcm[i] = alaw2linear(static_cast<unsigned char>(i));
}

[[maybe_unused]] void InitPcmAlawTable()
{ // 0x45af1c
  pcm16_alaw_tableinit();
  alaw_pcm16_tableinit();
}

// 0x45af38 — the original read the buffer as native (little-endian) 16-bit samples.
[[maybe_unused]] int PCM2G711a(const unsigned char* pcm, unsigned char* out, int bytes)
{
  if (!pcm || !out || bytes < 1)
  {
    std::cout << "Error, empty data or transmit failed, exit !\n";
    return -1;
  }
  const int n = bytes / 2;
  for (std::size_t i = 0; std::cmp_less(i, n); ++i)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): a 16-bit sample, 0..65535
    out[i] = g_pcm2alaw[pcm[2 * i] | (pcm[(2 * i) + 1] << 8)];
  return n;
}

// 0x45b008 — writes native (little-endian) 16-bit samples.
[[maybe_unused]] int G711a2PCM(const unsigned char* alaw, unsigned char* out, int n)
{
  if (!alaw || !out || n < 1)
  {
    std::cout << "Error, empty data or transmit failed, exit !\n";
    return -1;
  }
  for (std::size_t i = 0; std::cmp_less(i, n); ++i)
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): an A-law byte, 0..255
    const auto s = static_cast<unsigned short>(g_alaw2pcm[alaw[i]]);
    out[2 * i] = static_cast<unsigned char>(s & 0xff);
    out[(2 * i) + 1] = static_cast<unsigned char>(s >> 8);
  }
  return n;
}
} // namespace

// 0x45a610 — "YYYYmmddHHMMSSmmm"
std::string getCurrentSystemTimeChrono()
{
  const auto now = std::chrono::system_clock::now();
  const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() -
                  (std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() * 1000);
  const time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << tm.tm_year + 1900 << std::setw(2) << tm.tm_mon + 1 << std::setw(2)
      << tm.tm_mday << std::setw(2) << tm.tm_hour << std::setw(2) << tm.tm_min << std::setw(2) << tm.tm_sec
      << std::setw(3) << ms;
  return out.str();
}

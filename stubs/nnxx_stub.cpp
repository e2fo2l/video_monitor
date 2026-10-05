// Host stand-in for libnnxx/libnanomsg. Instead of a real NN_PAIR socket, "ipc://<path>" maps to
//   <path>.in   FIFO; write one JSON message per line to inject it
//   <path>.out  everything video_monitor sends, one message per line
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <vendor/nnxx.h>

namespace nnxx
{

namespace
{
std::string g_pending;
int g_outFd = -1;
} // namespace

message::message(std::size_t size, int /*unused*/)
    : m_data(std::malloc((size != 0U) ? size : 1))
    , m_size(size)
{
}
// NOLINTNEXTLINE(cppcoreguidelines-owning-memory): message owns a malloc'd buffer, like nn_allocmsg()
message::~message() { std::free(m_data); }
void* message::data() noexcept { return m_data; }
const char* message::begin() const noexcept { return static_cast<const char*>(m_data); }
const char* message::end() const noexcept { return static_cast<const char*>(m_data) + m_size; }
bool message::empty() const noexcept { return m_size == 0; }

socket::socket() noexcept = default;
socket::socket(int /*unused*/, int /*unused*/) {}
socket::socket(socket&& o) noexcept
    : m_fd(o.m_fd)
{
  o.m_fd = -1;
}
socket& socket::operator=(socket&& o) noexcept
{
  if (this != &o)
  {
    if (m_fd >= 0)
      ::close(m_fd);
    m_fd = o.m_fd;
    o.m_fd = -1;
  }
  return *this;
}
socket::~socket()
{
  if (m_fd >= 0)
    ::close(m_fd);
}

int socket::connect(const char* addr)
{
  std::string path = addr;
  if (path.starts_with("ipc://"))
    path = path.substr(6);
  const std::string in = path + ".in";
  const std::string out = path + ".out";
  mkfifo(in.c_str(), 0666);
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg): open() is variadic (optional mode)
  m_fd = ::open(in.c_str(), O_RDWR | O_NONBLOCK); // RDWR keeps the FIFO open with no writer
  g_outFd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  std::cerr << "[nnxx-stub] inject messages via " << in << ", replies in " << out << '\n';
  return m_fd >= 0 ? 0 : -1;
}

// NOLINTNEXTLINE(readability-make-member-function-const): must match libnnxx's non-const signature
message socket::recv(int /*unused*/)
{
  char buf[4096];
  for (ssize_t n = 0; m_fd >= 0 && (n = ::read(m_fd, buf, sizeof buf)) > 0;)
    g_pending.append(buf, static_cast<size_t>(n));
  const size_t nl = g_pending.find('\n');
  if (nl == std::string::npos)
    return {};
  const std::string line = g_pending.substr(0, nl);
  g_pending.erase(0, nl + 1);
  message m(line.size(), 0);
  std::memcpy(m.data(), line.data(), line.size());
  return m;
}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): libnnxx's signature; the message is only read
int socket::send(message&& msg, int /*unused*/)
{
  const std::string s(msg.begin(), msg.end());
  if (g_outFd >= 0)
  {
    const std::string line = s + "\n";
    if (::write(g_outFd, line.data(), line.size()) < 0)
      return -1;
  }
  return static_cast<int>(s.size());
}

} // namespace nnxx

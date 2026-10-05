// Minimal declaration of the nnxx (C++ nanomsg wrapper) API surface video_monitor links against.
// Signatures match the mangled imports of the original binary, e.g.
//   _ZN4nnxx6socketC1Eii, _ZN4nnxx6socket4recvEi, _ZN4nnxx6socket4sendEONS_7messageEi
#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace nnxx
{

constexpr int SP = 1;       // AF_SP
constexpr int PAIR = 16;    // NN_PAIR
constexpr int DONTWAIT = 1; // NN_DONTWAIT

class message
{
public:
  message() noexcept = default;
  message(std::size_t size, int type);
  message(message&& other) noexcept
      : m_data(other.m_data)
      , m_size(other.m_size)
  {
    other.m_data = nullptr;
    other.m_size = 0;
  }
  message(const message&) = delete;
  message& operator=(const message&) = delete;
  ~message();

  void* data() noexcept;
  const char* begin() const noexcept;
  const char* end() const noexcept;
  bool empty() const noexcept;

private:
  void* m_data = nullptr;
  std::size_t m_size = 0;
};

class socket
{
public:
  socket() noexcept;
  socket(int domain, int protocol);
  socket(socket&& other) noexcept;
  socket(const socket&) = delete;
  socket& operator=(socket&& other) noexcept;
  ~socket();

  int connect(const char* addr);
  message recv(int flags = 0);
  int send(message&& msg, int flags = 0);

private:
  int m_fd = -1;
};

// Header-only helper in upstream nnxx (inlined into video_monitor).
inline message make_message(const std::string& s)
{
  message m(s.size(), 0);
  std::memcpy(m.data(), s.data(), s.size());
  return m;
}

} // namespace nnxx

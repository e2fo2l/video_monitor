// Host stand-in for libagora-rtc-sdk.so.
//   VM_STUB_AGORA_OUT=<file>  append every H.264 buffer passed to agora_rtc_send_video_data
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>

#include <vendor/agora_rtc_api.h>

namespace
{
const agora_rtc_event_handler_t* g_handler = nullptr;
std::unique_ptr<std::ofstream> g_out;
} // namespace

extern "C"
{

  int agora_rtc_license_verify(const char* /*unused*/, int /*unused*/, const char* /*unused*/, int /*unused*/)
  {
    return 0;
  }

  int agora_rtc_init(const char* app_id, const agora_rtc_event_handler_t* event_handler, rtc_service_option_t* option)
  {
    g_handler = event_handler;
    std::cerr << "[agora-stub] init appid=" << (app_id ? app_id : "") << " area=0x" << std::hex
              << (option ? option->area_code : 0) << std::dec << '\n';
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the process modifies the environment
    if (const char* p = std::getenv("VM_STUB_AGORA_OUT"))
      g_out = std::make_unique<std::ofstream>(p, std::ios::binary);
    return 0;
  }

  int agora_rtc_fini(void) { return 0; }
  int agora_rtc_set_log_level(int /*unused*/) { return 0; }
  int agora_rtc_config_log(int /*unused*/, int /*unused*/) { return 0; }

  int agora_rtc_set_params(const char* params)
  {
    std::cerr << "[agora-stub] set_params " << params << '\n';
    return 0;
  }

  int agora_rtc_join_channel(const char* channel, const char* uid, const char* /*unused*/, size_t /*unused*/,
                             rtc_channel_options_t* /*unused*/)
  {
    std::cerr << "[agora-stub] join " << channel << " as " << uid << '\n';
    if (g_handler && g_handler->on_join_channel_success)
      g_handler->on_join_channel_success(channel, 0);
    return 0;
  }

  int agora_rtc_leave_channel(const char* channel)
  {
    std::cerr << "[agora-stub] leave " << channel << '\n';
    return 0;
  }

  int agora_rtc_send_video_data(const char* /*unused*/, uint8_t /*unused*/, const void* data, size_t len,
                                video_frame_info_t* /*unused*/)
  {
    if (g_out)
    {
      g_out->write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
      g_out->flush();
    }
    return 0;
  }

  const char* agora_rtc_err_2_str(int err) { return (err != 0) ? "stub error" : "ok"; }

} // extern "C"

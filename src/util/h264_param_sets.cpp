#include "h264_param_sets.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

class BitWriter
{
public:
  void u(unsigned bits, uint32_t value)
  {
    for (int i = static_cast<int>(bits) - 1; i >= 0; --i)
      bit((value >> i) & 1);
  }
  void ue(uint32_t value)
  { // Exp-Golomb
    const uint32_t v = value + 1;
    int len = 0;
    while ((v >> len) > 1)
      ++len;
    u(static_cast<unsigned>(len), 0);
    u(static_cast<unsigned>(len + 1), v);
  }
  void se(int32_t value)
  {
    ue(value <= 0 ? static_cast<uint32_t>(-2 * value) : static_cast<uint32_t>((2 * value) - 1));
  }
  std::vector<uint8_t> finish()
  { // rbsp_trailing_bits
    bit(1);
    while ((m_nbits % 8) != 0U)
      bit(0);
    return m_bytes;
  }

private:
  void bit(unsigned b)
  {
    if (m_nbits % 8 == 0)
      m_bytes.push_back(0);
    if (b != 0U)
      m_bytes.back() |= static_cast<uint8_t>(0x80 >> (m_nbits % 8));
    ++m_nbits;
  }
  std::vector<uint8_t> m_bytes;
  size_t m_nbits = 0;
};

// Start code + NAL header + RBSP with emulation-prevention bytes.
void appendNal(std::vector<uint8_t>& out, uint8_t header, const std::vector<uint8_t>& rbsp)
{
  out.insert(out.end(), {0x00, 0x00, 0x00, 0x01, header});
  int zeros = 0;
  for (const uint8_t b : rbsp)
  {
    if (zeros >= 2 && b <= 3)
    {
      out.push_back(0x03);
      zeros = 0;
    }
    out.push_back(b);
    zeros = (b == 0) ? zeros + 1 : 0;
  }
}

} // namespace

std::vector<uint8_t> BuildSpsPps(int width, int height)
{
  width = (width + 1) & ~1;
  height = (height + 1) & ~1;
  const auto mbW = static_cast<uint32_t>((width + 15) / 16);
  const auto mbH = static_cast<uint32_t>((height + 15) / 16);
  const uint32_t cropRight = ((mbW * 16) - static_cast<uint32_t>(width)) / 2;
  const uint32_t cropBottom = ((mbH * 16) - static_cast<uint32_t>(height)) / 2;

  BitWriter sps;
  sps.u(8, 77);    // profile_idc: Main
  sps.u(8, 0);     // constraint_set0..5_flag, reserved_zero_2bits
  sps.u(8, 31);    // level_idc 3.1 (fixed by the encoder, even where the size exceeds it)
  sps.ue(0);       // seq_parameter_set_id
  sps.ue(4);       // log2_max_frame_num_minus4      -> 8 bits
  sps.ue(0);       // pic_order_cnt_type
  sps.ue(4);       // log2_max_pic_order_cnt_lsb_minus4 -> 8 bits
  sps.ue(1);       // max_num_ref_frames
  sps.u(1, 0);     // gaps_in_frame_num_value_allowed_flag
  sps.ue(mbW - 1); // pic_width_in_mbs_minus1
  sps.ue(mbH - 1); // pic_height_in_map_units_minus1
  sps.u(1, 1);     // frame_mbs_only_flag
  sps.u(1, 0);     // direct_8x8_inference_flag
  const bool crop = (cropRight != 0U) || (cropBottom != 0U);
  sps.u(1, static_cast<uint32_t>(crop)); // frame_cropping_flag
  if (crop)
  {
    sps.ue(0);          // frame_crop_left_offset
    sps.ue(cropRight);  // frame_crop_right_offset
    sps.ue(0);          // frame_crop_top_offset
    sps.ue(cropBottom); // frame_crop_bottom_offset
  }
  sps.u(1, 0); // vui_parameters_present_flag

  BitWriter pps;
  pps.ue(0);   // pic_parameter_set_id
  pps.ue(0);   // seq_parameter_set_id
  pps.u(1, 1); // entropy_coding_mode_flag: CABAC
  pps.u(1, 0); // bottom_field_pic_order_in_frame_present_flag
  pps.ue(0);   // num_slice_groups_minus1
  pps.ue(0);   // num_ref_idx_l0_default_active_minus1
  pps.ue(0);   // num_ref_idx_l1_default_active_minus1
  pps.u(1, 0); // weighted_pred_flag
  pps.u(2, 0); // weighted_bipred_idc
  pps.se(0);   // pic_init_qp_minus26
  pps.se(0);   // pic_init_qs_minus26
  pps.se(0);   // chroma_qp_index_offset
  pps.u(1, 1); // deblocking_filter_control_present_flag
  pps.u(1, 0); // constrained_intra_pred_flag
  pps.u(1, 0); // redundant_pic_cnt_present_flag

  std::vector<uint8_t> out;
  appendNal(out, 0x67, sps.finish());
  appendNal(out, 0x68, pps.finish());
  return out;
}

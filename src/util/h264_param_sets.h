// Builds the Annex-B SPS+PPS pair the Allwinner encoder (libvencoder via libtrecorder) produces,
// for an arbitrary output size.
//
// The five blobs hard-coded in the original video_monitor (180/360/480/504/720) decode to the
// same parameters except for the frame size; see BuildSpsPps() for the full field list.
#pragma once

#include <cstdint>
#include <vector>

// Returns "00 00 00 01 <SPS> 00 00 00 01 <PPS>". Odd dimensions are rounded up to the next
// even value (4:2:0 cropping works in 2-pixel units).
std::vector<uint8_t> BuildSpsPps(int width, int height);

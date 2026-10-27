#pragma once
// The RK3568's (and RK3566's) VOP2 frame length: VPn's DSP_VTOTAL_VS_END,
// with its undocumented immediate bit, written through /dev/mem. A write with
// the bit set applies to the frame being scanned out and holds until the
// next write.

#include <cstdint>

#include "hal/frame_length.hpp"

namespace vop2 {

constexpr uint32_t kBase      = 0xfe040000u;   // RK3568 / RK3566
constexpr uint32_t kVtotalReg = 0x0c50;        // VP0's DSP_VTOTAL_VS_END, VPn's 0x100 further on each
constexpr uint32_t kImd       = 1u << 15;      // applies to the frame being scanned out

// The register's fields: vtotal in [28:16], the sync width in [12:0].
constexpr int vtotal_of(uint32_t v) { return (int)((v >> 16) & 0x1fff); }
constexpr int sync_of(uint32_t v)   { return (int)(v & 0x1fff); }

// Whether `v` (immediate bit clear) is a mode of `vtotal` lines and
// `sync_lines` of sync. Off by a little is ours, left by a run that did not
// get to put it back.
constexpr bool is_mode(uint32_t v, int vtotal, int sync_lines) {
    return sync_of(v) == sync_lines && vtotal_of(v) >= vtotal - 1 && vtotal_of(v) <= 2 * vtotal;
}
// The mode's own value from what the register held.
constexpr uint32_t mode_value(uint32_t v, int vtotal) {
    return (v & ~(0x1fffu << 16)) | ((uint32_t)vtotal << 16);
}
// `orig` with the total set to `total` lines, applied at once.
constexpr uint32_t total_value(uint32_t orig, int total) {
    return (orig & ~(0x1fffu << 16)) | (((uint32_t)total & 0x1fff) << 16) | kImd;
}

// Whether a device tree (compatible list, spaces between) has this VOP2.
bool on_this_soc(const char *compat);

}  // namespace vop2

namespace hal {
std::unique_ptr<FrameLength> map_vop2_frame_length(int crtc_index);
}

#include "hal/frame_length.hpp"

#include <cstdio>

#include "kestrel_gnd_features.h"
#if KESTREL_HAL_ROCKCHIP
#include "hal/rockchip/vop2_frame_length.hpp"
#endif

namespace hal {

std::unique_ptr<FrameLength> map_frame_length(int crtc_index) {
#if KESTREL_HAL_ROCKCHIP
    return map_vop2_frame_length(crtc_index);
#else
    (void)crtc_index;
    printf("display lock: no frame-length control in this build - Refresh Sync is not available\n");
    return nullptr;
#endif
}

}  // namespace hal

#include "hal/plane_rules.hpp"

#include "kestrel_gnd_features.h"

namespace hal {

const PlaneRules &plane_rules() {
#if KESTREL_HAL_ROCKCHIP
    // RK3568 VOP2: YUV planes want 2-pixel alignment, and a plane under 4x4
    // is made invisible (it says so on every commit).
    static constexpr PlaneRules r{2, 4};
#else
    // No particular controller: the same, which any YUV 4:2:0 plane needs.
    static constexpr PlaneRules r{2, 4};
#endif
    return r;
}

}  // namespace hal

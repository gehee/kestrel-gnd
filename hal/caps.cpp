#include "hal/caps.hpp"

#include <cstdio>

#include "core/wifi_ap.hpp"
#include "hal/screen_out.hpp"
#include "kestrel_gnd_features.h"

namespace hal {

static Caps work_out() {
    Caps c;
    c.stream_decode = KESTREL_STREAM_DECODE;
    c.phase_lock = KESTREL_HAL_ROCKCHIP;   // the VOP2's frame length
    c.screen_enc = KESTREL_SCREEN_ENC && screen_compositor_available();
    c.wifi_ap = wifi_ap_available();
    printf("caps: stream decode %s, refresh sync %s, screen recording %s, wifi %s\n",
           c.stream_decode ? "yes" : "no", c.phase_lock ? "yes" : "no",
           c.screen_enc ? "yes" : "no", c.wifi_ap ? "yes" : "no");
    return c;
}

const Caps &caps() {
    static const Caps c = work_out();
    return c;
}

}  // namespace hal

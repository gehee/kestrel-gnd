// Unit test of hal::caps(): each capability follows the build's features and
// what the unit has, and it is worked out once.
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <cstdio>
#include <unistd.h>

#include "hal/caps.hpp"
#include "kestrel_gnd_features.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
    const hal::Caps &c = hal::caps();
    CHECK(&hal::caps() == &c);                                   // once
    CHECK(c.stream_decode == (bool)KESTREL_STREAM_DECODE);
    if (!KESTREL_SCREEN_ENC) CHECK(!c.screen_enc);              // a feature left out is never there
    if (!KESTREL_WIFI) CHECK(!c.wifi_ap);
    // The access point needs fpvOS's script; no script, no WiFi row.
    if (access("/usr/sbin/fpvos-wifi", X_OK) != 0) CHECK(!c.wifi_ap);
    // Refresh Sync only on a display controller the HAL can drive.
    CHECK(c.phase_lock == (bool)KESTREL_HAL_ROCKCHIP);
    std::printf(fails ? "caps_test: %d FAILED\n" : "caps_test: all passed\n", fails);
    return fails ? 1 : 0;
}

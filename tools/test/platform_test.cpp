// Unit test of the platform (platform/platform.hpp), as this build's box has
// it: what it can do follows the build's features and what the unit has, and
// is worked out once; which decoder a name gets; the screen's encoder and
// compositor it hands out, stand-ins where the box has none. Nothing here
// opens a display, MPP or RGA.
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "kestrel_gnd_features.h"
#include "platform/platform.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void test_caps() {
    const platform::Caps &c = platform::caps();
    CHECK(&platform::caps() == &c);                              // once
    CHECK(c.stream_decode == (bool)KESTREL_STREAM_DECODE);
    if (!KESTREL_SCREEN_ENC) CHECK(!c.screen_enc);              // a feature left out is never there
    if (!KESTREL_WIFI) CHECK(!c.wifi_ap);
    // The access point needs fpvOS's script; no script, no WiFi row.
    if (access("/usr/sbin/fpvos-wifi", X_OK) != 0) CHECK(!c.wifi_ap);
    // Refresh Sync only on a display controller the box can drive.
    CHECK(c.phase_lock == (bool)KESTREL_BOARD_VRXPRO);
}

static void test_decoders() {
    CHECK(std::strcmp(platform::name(), KESTREL_BOARD_VRXPRO ? "vrxpro" : "pc") == 0);
    CHECK(platform::hardware_decoder("rkmpp") == (bool)KESTREL_BOARD_VRXPRO);
    CHECK(!platform::hardware_decoder(""));
    CHECK(!platform::hardware_decoder("hevc_cuvid"));
    CHECK(platform::has_background_decoder() == (bool)KESTREL_BOARD_VRXPRO);
}

static void test_screen_out() {
    auto enc = platform::make_screen_encoder();
    auto comp = platform::make_screen_compositor();
    CHECK(enc != nullptr);
    CHECK(comp != nullptr);
    // Not initialised: nothing to encode from, whichever encoder it is.
    CHECK(!enc->ready());
    CHECK(enc->input_count() == 0);
    CHECK(enc->input_buffer(0) == nullptr);
    enc->set_hevc(true);
    CHECK(enc->hevc());
    if (!(KESTREL_BOARD_VRXPRO && KESTREL_SCREEN_ENC)) {
        // No encoder on this box: init refuses, as the recorder expects.
        CHECK(!enc->init(1920, 1080, 60, 16000000));
        CHECK(!comp->init(1920, 1080, {}));
    }
    if (!KESTREL_BOARD_VRXPRO) CHECK(!platform::screen_compositor_available());
}

int main() {
    test_caps();
    test_decoders();
    test_screen_out();
    std::printf(fails ? "platform_test: %d FAILED\n" : "platform_test: all passed\n", fails);
    return fails ? 1 : 0;
}

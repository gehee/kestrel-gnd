// Unit test of the HAL pieces that can be checked without the hardware: the
// VOP2 frame-length register's arithmetic, which decoder a name gets, and
// what the screen-output and frame-length factories hand out in this build.
// Nothing here opens /dev/mem, MPP or RGA.
//   cmake -DKESTREL_TESTS=ON ... && ctest     (or run the binary on the unit)
#include <cstdio>
#include <string>

#include "hal/decoder.hpp"
#include "hal/frame_length.hpp"
#include "hal/plane_rules.hpp"
#include "hal/rockchip/vop2_frame_length.hpp"
#include "hal/screen_out.hpp"
#include "kestrel_gnd_features.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void test_vop2_register() {
    // A 1080p120 mode on the CPO1F41: vtotal 1125, sync 5 lines. The
    // register holds vtotal in [28:16] and the sync width in [12:0].
    const uint32_t reg = (1125u << 16) | 5;
    CHECK(vop2::vtotal_of(reg) == 1125);
    CHECK(vop2::sync_of(reg) == 5);
    CHECK(vop2::is_mode(reg, 1125, 5));
    CHECK(!vop2::is_mode(reg, 1125, 6));             // another mode's sync
    CHECK(!vop2::is_mode(reg, 500, 5));              // more than twice a 500-line mode
    CHECK(vop2::is_mode((1124u << 16) | 5, 1125, 5)); // one short: ours, left behind
    CHECK(!vop2::is_mode((1123u << 16) | 5, 1125, 5));
    CHECK(vop2::is_mode((2250u << 16) | 5, 1125, 5));
    CHECK(!vop2::is_mode((2251u << 16) | 5, 1125, 5));

    // A total left at 1130 by an earlier run comes back as the mode's 1125.
    const uint32_t orig = vop2::mode_value((1130u << 16) | 5, 1125);
    CHECK(orig == ((1125u << 16) | 5));

    // A longer frame: total written, sync kept, immediate bit set.
    const uint32_t w = vop2::total_value(orig, 1125 + 20);
    CHECK(vop2::vtotal_of(w) == 1145);
    CHECK(vop2::sync_of(w) == 5);
    CHECK((w & vop2::kImd) != 0);
    CHECK((orig & vop2::kImd) == 0);                 // restoring clears it
    // Shorter, and a total too big for the field is masked, not spilled.
    CHECK(vop2::vtotal_of(vop2::total_value(orig, 1105)) == 1105);
    CHECK((vop2::total_value(orig, 0x2000 + 7) & ~(0x1fffu << 16) & ~vop2::kImd) == 5);

    CHECK(vop2::on_this_soc("rockchip,rk3568-evb1-ddr4-v10 rockchip,rk3568 "));
    CHECK(vop2::on_this_soc("rockchip,rk3566 "));
    CHECK(!vop2::on_this_soc("rockchip,rk3588 "));
    CHECK(!vop2::on_this_soc("artosyn,proxima-9311 "));
    CHECK(!vop2::on_this_soc(""));
}

static void test_decoder_choice() {
    using hal::DecoderBackend;
    CHECK(hal::decoder_backend("rkmpp") ==
          (KESTREL_HAL_ROCKCHIP ? DecoderBackend::Rockchip : DecoderBackend::Ffmpeg));
    CHECK(hal::decoder_backend("") == DecoderBackend::Ffmpeg);
    CHECK(hal::decoder_backend("hevc_cuvid") == DecoderBackend::Ffmpeg);
    CHECK(hal::has_background_decoder() == (bool)KESTREL_HAL_ROCKCHIP);
}

static void test_screen_out() {
    auto enc = hal::make_screen_encoder();
    auto comp = hal::make_screen_compositor();
    CHECK(enc != nullptr);
    CHECK(comp != nullptr);
    // Not initialised: nothing to encode from, whichever encoder it is.
    CHECK(!enc->ready());
    CHECK(enc->input_count() == 0);
    CHECK(enc->input_buffer(0) == nullptr);
    enc->set_hevc(true);
    CHECK(enc->hevc());
#if !(KESTREL_HAL_ROCKCHIP && KESTREL_SCREEN_ENC)
    // No encoder in this build: init refuses, as the recorder expects.
    CHECK(!enc->init(1920, 1080, 60, 16000000));
    CHECK(!comp->init(1920, 1080, {}));
    CHECK(!hal::screen_compositor_available());
#endif
}

static void test_plane_rules() {
    const hal::PlaneRules &r = hal::plane_rules();
    // Every controller here takes YUV 4:2:0, so at least 2-pixel alignment,
    // and a power of two for down() to be a mask.
    CHECK(r.align >= 2 && (r.align & (r.align - 1)) == 0);
    CHECK(r.min_rows >= 1);
#if KESTREL_HAL_ROCKCHIP
    CHECK(r.align == 2 && r.min_rows == 4);   // the VOP2's
#endif
    CHECK(r.down(1920) == 1920);
    CHECK(r.down(1081) == 1080);
    CHECK(r.down(0) == 0);
    CHECK(r.down(1u) == 0u);
    CHECK(r.down(541u) == 540u);
    // Picture Size 85% of 1080p, as DrmDevice::picture_rect rounds it.
    CHECK(r.down(1920 * 85 / 100) == 1632);
    CHECK(r.down(1080 * 85 / 100) == 918);
}

static void test_frame_length() {
#if !KESTREL_HAL_ROCKCHIP
    CHECK(hal::map_frame_length(0) == nullptr);
#endif
}

int main() {
    test_vop2_register();
    test_decoder_choice();
    test_screen_out();
    test_plane_rules();
    test_frame_length();
    std::printf(fails ? "hal_test: %d FAILED\n" : "hal_test: all passed\n", fails);
    return fails ? 1 : 0;
}

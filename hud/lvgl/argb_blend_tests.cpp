#include "argb_blend.h"
#include "src/misc/lv_color_op.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

static uint32_t reference(uint32_t f, uint32_t b) {
    lv_color32_t fg, bg;
    std::memcpy(&fg, &f, 4); std::memcpy(&bg, &b, 4);
    if (fg.alpha >= LV_OPA_MAX || bg.alpha <= LV_OPA_MIN) return f;
    if (fg.alpha <= LV_OPA_MIN) return b;
    if (bg.alpha == 255) fg = lv_color_mix32(fg, bg);
    else {
        unsigned alpha = 255 - LV_OPA_MIX2(255 - fg.alpha, 255 - bg.alpha);
        fg.alpha = unsigned(fg.alpha) * 255 / alpha;
        fg = lv_color_mix32(fg, bg); fg.alpha = alpha;
    }
    std::memcpy(&f, &fg, 4); return f;
}
static void check(bool ok) { if (!ok) throw std::runtime_error("ARGB blend mismatch"); }
int main() {
    try {
        // Every source/destination alpha pair, unaligned pixel starts, tails,
        // row padding, constant and changing colors, and all opacity branches.
        const int width = 65539, stride = width + 5, height = 2;
        std::vector<uint32_t> src(stride * height + 2), bg(src.size()), dst;
        uint32_t rng = 7;
        for (int pattern = 0; pattern < 3; ++pattern) {
            for (size_t i = 0; i < src.size(); ++i) {
                rng = rng * 1664525 + 1013904223;
                src[i] = (uint32_t((i / 256) & 255) << 24) | (pattern ? rng & 0xffffff : 0x123456);
                rng = rng * 1664525 + 1013904223;
                bg[i] = (uint32_t(i & 255) << 24) | (pattern ? rng & 0xffffff : 0xabcdef);
            }
            for (int opacity : {0, 1, 2, 3, 127, 252, 253, 254, 255}) {
                dst = bg;
                lv_draw_sw_blend_image_dsc_t d{};
                d.src_buf = src.data() + 1; d.dest_buf = dst.data() + 1;
                d.dest_w = width; d.dest_h = height;
                d.src_stride = d.dest_stride = stride * 4; d.opa = opacity;
                d.blend_mode = LV_BLEND_MODE_NORMAL;
                check(kestrel_argb_image(&d) == LV_RESULT_OK);
                for (int y = 0; y < height; ++y) for (int x = 0; x < stride; ++x) {
                    size_t at = 1 + y * stride + x;
                    uint32_t f = src[at];
                    if (opacity < LV_OPA_MAX) f = (f & 0xffffff) | ((((f >> 24) * opacity) >> 8) << 24);
                    check(dst[at] == (x < width ? reference(f, bg[at]) : bg[at]));
                }
                check(dst.front() == bg.front() && dst.back() == bg.back());
                uint8_t mask = 0; d.mask_buf = &mask;
                check(kestrel_argb_image(&d) == LV_RESULT_INVALID);
            }
        }
        for (int alpha = 0; alpha < 256; ++alpha) {
            dst = bg;
            lv_draw_sw_blend_fill_dsc_t d{};
            d.dest_buf = dst.data() + 1; d.dest_w = 263; d.dest_h = 2;
            d.dest_stride = stride * 4; d.color = lv_color_make(19, 77, 230); d.opa = alpha;
            check(kestrel_argb_fill(&d) == LV_RESULT_OK);
            for (int y = 0; y < 2; ++y) for (int x = 0; x < 264; ++x) {
                size_t at = 1 + y * stride + x;
                check(dst[at] == (x < 263 ? reference(uint32_t(alpha) << 24 | 0x134de6, bg[at]) : bg[at]));
            }
        }
        puts("ARGB blend matches LVGL across all alpha pairs, opacity thresholds, strides and tails");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}

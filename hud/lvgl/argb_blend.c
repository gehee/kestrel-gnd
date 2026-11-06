/* LVGL 9.2 straight-alpha composition, including its opacity thresholds and
 * integer rounding. Only unmasked normal fills/images use this fast path;
 * LVGL retains all other formats, masks and blend modes.
 */
#include "argb_blend.h"
#include <stdint.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

static inline uint32_t mix(uint32_t f, uint32_t b)
{
    unsigned a = f >> 24, ba = b >> 24;
    if (a >= LV_OPA_MAX || ba <= LV_OPA_MIN) return f;
    if (a <= LV_OPA_MIN) return b;
    unsigned out = 255 - (((255 - a) * (255 - ba)) >> 8);
    unsigned ratio = a * 255 / out;
    uint32_t rgb;
    if (ratio >= LV_OPA_MAX) rgb = f;
    else if (ratio <= LV_OPA_MIN) rgb = b;
    else {
        unsigned inv = 255 - ratio;
        rgb = ((((f & 0xff00ff) * ratio + (b & 0xff00ff) * inv) >> 8) & 0xff00ff) |
              ((((f & 0xff00) * ratio + (b & 0xff00) * inv) >> 8) & 0xff00);
    }
    return (rgb & 0xffffff) | (out << 24);
}
#ifdef __aarch64__
static inline uint32x4_t mix4(uint32x4_t f, uint32x4_t b)
{
    const uint32x4_t ff = vdupq_n_u32(255), mask = vdupq_n_u32(0xffffff);
    uint32x4_t a = vshrq_n_u32(f, 24), ba = vshrq_n_u32(b, 24);
    /* Uniform areas dominate panels. These also avoid any division for a
     * transparent destination or completely transparent/opaque source span. */
    if (vminvq_u32(a) >= LV_OPA_MAX || vmaxvq_u32(ba) <= LV_OPA_MIN) return f;
    if (vmaxvq_u32(a) <= LV_OPA_MIN && vminvq_u32(ba) > LV_OPA_MIN) return b;
    uint32x4_t out = vsubq_u32(ff, vshrq_n_u32(vmulq_u32(vsubq_u32(ff, a), vsubq_u32(ff, ba)), 8));
    uint32x4_t num = vmulq_u32(a, ff);
    float32x4_t den = vcvtq_f32_u32(out);
    float32x4_t reciprocal = vrecpeq_f32(den);
    reciprocal = vmulq_f32(reciprocal, vrecpsq_f32(den, reciprocal));
    reciprocal = vmulq_f32(reciprocal, vrecpsq_f32(den, reciprocal));
    uint32x4_t ratio = vcvtq_u32_f32(vmulq_f32(vcvtq_f32_u32(num), reciprocal));
    /* Correct either rounding direction to the exact integer quotient. */
    ratio = vsubq_u32(ratio, vandq_u32(vcgtq_u32(vmulq_u32(ratio, out), num), vdupq_n_u32(1)));
    ratio = vaddq_u32(ratio, vandq_u32(vcgeq_u32(vsubq_u32(num, vmulq_u32(ratio, out)), out), vdupq_n_u32(1)));
    uint32x4_t inv = vsubq_u32(ff, ratio);
    uint32x4_t rb = vandq_u32(vshrq_n_u32(vaddq_u32(vmulq_u32(vandq_u32(f, vdupq_n_u32(0xff00ff)), ratio),
        vmulq_u32(vandq_u32(b, vdupq_n_u32(0xff00ff)), inv)), 8), vdupq_n_u32(0xff00ff));
    uint32x4_t g = vandq_u32(vshrq_n_u32(vaddq_u32(vmulq_u32(vandq_u32(f, vdupq_n_u32(0xff00)), ratio),
        vmulq_u32(vandq_u32(b, vdupq_n_u32(0xff00)), inv)), 8), vdupq_n_u32(0xff00));
    uint32x4_t rgb = vorrq_u32(rb, g);
    rgb = vbslq_u32(vcgeq_u32(ratio, vdupq_n_u32(LV_OPA_MAX)), f, rgb);
    rgb = vbslq_u32(vcleq_u32(ratio, vdupq_n_u32(LV_OPA_MIN)), b, rgb);
    uint32x4_t result = vorrq_u32(vandq_u32(rgb, mask), vshlq_n_u32(out, 24));
    result = vbslq_u32(vcleq_u32(a, vdupq_n_u32(LV_OPA_MIN)), b, result);
    return vbslq_u32(vorrq_u32(vcgeq_u32(a, vdupq_n_u32(LV_OPA_MAX)),
                              vcleq_u32(ba, vdupq_n_u32(LV_OPA_MIN))), f, result);
}
#endif
lv_result_t kestrel_argb_fill(lv_draw_sw_blend_fill_dsc_t *d)
{
    if (d->mask_buf) return LV_RESULT_INVALID;
    uint32_t f = (uint32_t)d->opa << 24 | (uint32_t)d->color.red << 16 |
                 (uint32_t)d->color.green << 8 | d->color.blue;
    for (int y = 0; y < d->dest_h; ++y) {
        uint32_t *dst = (uint32_t *)((uint8_t *)d->dest_buf + y * d->dest_stride);
        int x = 0;
#ifdef __aarch64__
        for (; x + 4 <= d->dest_w; x += 4) vst1q_u32(dst + x, mix4(vdupq_n_u32(f), vld1q_u32(dst + x)));
#endif
        for (; x < d->dest_w; ++x) dst[x] = mix(f, dst[x]);
    }
    return LV_RESULT_OK;
}
lv_result_t kestrel_argb_image(lv_draw_sw_blend_image_dsc_t *d)
{
    if (d->mask_buf || d->blend_mode != LV_BLEND_MODE_NORMAL) return LV_RESULT_INVALID;
    for (int y = 0; y < d->dest_h; ++y) {
        uint32_t *dst = (uint32_t *)((uint8_t *)d->dest_buf + y * d->dest_stride);
        const uint32_t *src = (const uint32_t *)((const uint8_t *)d->src_buf + y * d->src_stride);
        int x = 0;
#ifdef __aarch64__
        for (; x + 4 <= d->dest_w; x += 4) {
            uint32x4_t f = vld1q_u32(src + x);
            if (d->opa < LV_OPA_MAX) f = vorrq_u32(vandq_u32(f, vdupq_n_u32(0xffffff)),
                vshlq_n_u32(vshrq_n_u32(vmulq_n_u32(vshrq_n_u32(f, 24), d->opa), 8), 24));
            vst1q_u32(dst + x, mix4(f, vld1q_u32(dst + x)));
        }
#endif
        for (; x < d->dest_w; ++x) {
            uint32_t f = src[x];
            if (d->opa < LV_OPA_MAX) f = (f & 0xffffff) | ((((f >> 24) * d->opa) >> 8) << 24);
            dst[x] = mix(f, dst[x]);
        }
    }
    return LV_RESULT_OK;
}

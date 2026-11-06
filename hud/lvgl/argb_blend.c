/* LVGL 9.2 straight-alpha composition, including its opacity thresholds and
 * integer rounding. Normal fills (with or without masks) and unmasked ARGB
 * images use this fast path; LVGL retains other formats and blend modes.
 */
#include "argb_blend.h"
#include <stdint.h>
#include <string.h>
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
typedef struct { uint32x4_t f, b, result; } mix_cache;
static inline uint32x4_t cached_mix4(uint32x4_t f, uint32x4_t b, mix_cache *cache)
{
    /* Flat panel spans repeat the same four pixels for hundreds of blocks.
     * Retain the exact result, including alpha rounding, across those spans. */
    if(vminvq_u32(vandq_u32(vceqq_u32(f, cache->f), vceqq_u32(b, cache->b))))
        return cache->result;
    cache->f = f; cache->b = b; cache->result = mix4(f, b);
    return cache->result;
}
#endif
lv_result_t kestrel_argb_fill(lv_draw_sw_blend_fill_dsc_t *d)
{
    uint32_t f = (uint32_t)d->opa << 24 | (uint32_t)d->color.red << 16 |
                 (uint32_t)d->color.green << 8 | d->color.blue;
#ifdef __aarch64__
    mix_cache cache = {vdupq_n_u32(0), vdupq_n_u32(0), vdupq_n_u32(0)};
#endif
    for (int y = 0; y < d->dest_h; ++y) {
        uint32_t *dst = (uint32_t *)((uint8_t *)d->dest_buf + y * d->dest_stride);
        const uint8_t *mask = d->mask_buf ? d->mask_buf + y * d->mask_stride : NULL;
        int x = 0;
#ifdef __aarch64__
        if(!mask) {
            const uint32x4_t fg = vdupq_n_u32(f);
            for(; x + 16 <= d->dest_w; x += 16) {
                const uint32x4_t b0 = vld1q_u32(dst+x), b1 = vld1q_u32(dst+x+4);
                const uint32x4_t b2 = vld1q_u32(dst+x+8), b3 = vld1q_u32(dst+x+12);
                const uint32x4_t result = cached_mix4(fg, b0, &cache);
                const uint32x4_t different = vorrq_u32(veorq_u32(b0,b1),
                    vorrq_u32(veorq_u32(b0,b2),veorq_u32(b0,b3)));
                vst1q_u32(dst+x,result);
                if(!vmaxvq_u32(different)) {
                    vst1q_u32(dst+x+4,result);vst1q_u32(dst+x+8,result);vst1q_u32(dst+x+12,result);
                } else {
                    vst1q_u32(dst+x+4,cached_mix4(fg,b1,&cache));
                    vst1q_u32(dst+x+8,cached_mix4(fg,b2,&cache));
                    vst1q_u32(dst+x+12,cached_mix4(fg,b3,&cache));
                }
            }
        }
        for (; x + 4 <= d->dest_w; x += 4) {
            uint32x4_t fg = vdupq_n_u32(f);
            if(mask) {
                uint32_t packed; memcpy(&packed, mask + x, 4);
                uint32x4_t a = vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(packed)))));
                if(d->opa < LV_OPA_MAX) a = vshrq_n_u32(vmulq_n_u32(a, d->opa), 8);
                fg = vorrq_u32(vdupq_n_u32(f & 0xffffff), vshlq_n_u32(a, 24));
            }
            vst1q_u32(dst + x, cached_mix4(fg, vld1q_u32(dst + x), &cache));
        }
#endif
        for (; x < d->dest_w; ++x) {
            uint32_t fg = f;
            if(mask) fg = (f & 0xffffff) | ((uint32_t)(d->opa >= LV_OPA_MAX ? mask[x] : (mask[x] * d->opa) >> 8) << 24);
            dst[x] = mix(fg, dst[x]);
        }
    }
    return LV_RESULT_OK;
}
lv_result_t kestrel_argb_image(lv_draw_sw_blend_image_dsc_t *d)
{
    if (d->mask_buf || d->blend_mode != LV_BLEND_MODE_NORMAL) return LV_RESULT_INVALID;
#ifdef __aarch64__
    mix_cache cache = {vdupq_n_u32(0), vdupq_n_u32(0), vdupq_n_u32(0)};
#endif
    for (int y = 0; y < d->dest_h; ++y) {
        uint32_t *dst = (uint32_t *)((uint8_t *)d->dest_buf + y * d->dest_stride);
        const uint32_t *src = (const uint32_t *)((const uint8_t *)d->src_buf + y * d->src_stride);
        int x = 0;
#ifdef __aarch64__
        for (; x + 4 <= d->dest_w; x += 4) {
            uint32x4_t f = vld1q_u32(src + x);
            if (d->opa < LV_OPA_MAX) f = vorrq_u32(vandq_u32(f, vdupq_n_u32(0xffffff)),
                vshlq_n_u32(vshrq_n_u32(vmulq_n_u32(vshrq_n_u32(f, 24), d->opa), 8), 24));
            vst1q_u32(dst + x, cached_mix4(f, vld1q_u32(dst + x), &cache));
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

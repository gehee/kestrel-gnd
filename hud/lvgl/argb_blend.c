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
uint32_t kestrel_argb_mix(uint32_t f, uint32_t b) { return mix(f, b); }
#ifdef __aarch64__
static inline uint32x4_t mix4(uint32x4_t f, uint32x4_t b)
{
    const uint32x4_t ff = vdupq_n_u32(255), mask = vdupq_n_u32(0xffffff);
    uint32x4_t a = vshrq_n_u32(f, 24), ba = vshrq_n_u32(b, 24);
    /* Uniform areas dominate panels. These also avoid any division for a
     * transparent destination or completely transparent/opaque source span. */
    if (vminvq_u32(a) >= LV_OPA_MAX || vmaxvq_u32(ba) <= LV_OPA_MIN) return f;
    if (vmaxvq_u32(a) <= LV_OPA_MIN && vminvq_u32(ba) > LV_OPA_MIN) return b;
    uint32x4_t out, ratio;
    if (vminvq_u32(ba) == 255) {
        /* Over an opaque destination (idle artwork, opaque panels) the result
         * is opaque and the source alpha is the ratio: no division. */
        out = ff; ratio = a;
    } else {
        out = vsubq_u32(ff, vshrq_n_u32(vmulq_u32(vsubq_u32(ff, a), vsubq_u32(ff, ba)), 8));
        uint32x4_t num = vmulq_u32(a, ff);
        float32x4_t den = vcvtq_f32_u32(out);
        float32x4_t reciprocal = vrecpeq_f32(den);
        reciprocal = vmulq_f32(reciprocal, vrecpsq_f32(den, reciprocal));
        reciprocal = vmulq_f32(reciprocal, vrecpsq_f32(den, reciprocal));
        ratio = vcvtq_u32_f32(vmulq_f32(vcvtq_f32_u32(num), reciprocal));
        /* Correct either rounding direction to the exact integer quotient. */
        ratio = vsubq_u32(ratio, vandq_u32(vcgtq_u32(vmulq_u32(ratio, out), num), vdupq_n_u32(1)));
        ratio = vaddq_u32(ratio, vandq_u32(vcgeq_u32(vsubq_u32(num, vmulq_u32(ratio, out)), out), vdupq_n_u32(1)));
    }
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
#ifdef __aarch64__
/* Sixteen pixels over an opaque destination (idle artwork, or anything
 * already blended over it). LVGL's blend then reduces to the source alpha as
 * the ratio and an opaque result, so it runs on byte planes without the
 * division. A source at LV_OPA_MAX or more replaces the pixel, its alpha too;
 * one at LV_OPA_MIN or less leaves it. d: destination planes B, G, R, A. */
static inline uint8x16_t mix_plane(uint8x16_t f, uint8x16_t b, uint8x16_t a, uint8x16_t inv)
{
    return vcombine_u8(vshrn_n_u16(vmlal_u8(vmull_u8(vget_low_u8(f), vget_low_u8(a)), vget_low_u8(b), vget_low_u8(inv)), 8),
                       vshrn_n_u16(vmlal_high_u8(vmull_high_u8(f, a), b, inv), 8));
}
static inline void over_opaque16(uint8x16x4_t *d, uint8x16_t fb, uint8x16_t fg, uint8x16_t fr, uint8x16_t a)
{
    const uint8x16_t inv = vmvnq_u8(a);
    const uint8x16_t replace = vcgeq_u8(a, vdupq_n_u8(LV_OPA_MAX)), keep = vcleq_u8(a, vdupq_n_u8(LV_OPA_MIN));
    d->val[0] = vbslq_u8(replace, fb, vbslq_u8(keep, d->val[0], mix_plane(fb, d->val[0], a, inv)));
    d->val[1] = vbslq_u8(replace, fg, vbslq_u8(keep, d->val[1], mix_plane(fg, d->val[1], a, inv)));
    d->val[2] = vbslq_u8(replace, fr, vbslq_u8(keep, d->val[2], mix_plane(fr, d->val[2], a, inv)));
    d->val[3] = vbslq_u8(replace, a, d->val[3]);
}
/* Whether 16 pixels are all opaque: the smallest value has the smallest alpha. */
static inline int opaque_at(const uint32_t *p)
{
    return vminvq_u32(vminq_u32(vminq_u32(vld1q_u32(p), vld1q_u32(p + 4)), vminq_u32(vld1q_u32(p + 8), vld1q_u32(p + 12)))) >= 0xff000000u;
}
/* Constant colors over opaque pixels: each translucent fill turns a channel
 * into (c * a + b * (255 - a)) >> 8 and keeps the pixel opaque; one at
 * LV_OPA_MIN or less leaves it. Two fills chain in 16 bits, each truncated
 * as LVGL truncates it. */
typedef struct { int n; uint16x8_t c[2][3]; uint8x16_t inv0; uint16x8_t inv1; } opaque_fills;
/* Fills `count` colors in order; false when one would replace the pixels
 * (LV_OPA_MAX or more), which the general path keeps. */
static inline int opaque_fills_init(opaque_fills *of, const uint32_t *colors, int count)
{
    of->n = 0;
    for (int k = 0; k < count; ++k) {
        const unsigned a = colors[k] >> 24;
        if (a >= LV_OPA_MAX) return 0;
        if (a <= LV_OPA_MIN) continue;
        for (int ch = 0; ch < 3; ++ch) of->c[of->n][ch] = vdupq_n_u16((uint16_t)(((colors[k] >> (8 * ch)) & 255) * a));
        if (of->n == 0) of->inv0 = vdupq_n_u8((uint8_t)(255 - a));
        else of->inv1 = vdupq_n_u16((uint16_t)(255 - a));
        ++of->n;
    }
    return 1;
}
static inline uint8x16_t opaque_fills_plane(uint8x16_t b, const opaque_fills *of, int ch)
{
    if (of->n == 0) return b;
    uint16x8_t lo = vmlal_u8(of->c[0][ch], vget_low_u8(b), vget_low_u8(of->inv0));
    uint16x8_t hi = vmlal_high_u8(of->c[0][ch], b, of->inv0);
    if (of->n == 2) {
        lo = vmlaq_u16(of->c[1][ch], vshrq_n_u16(lo, 8), of->inv1);
        hi = vmlaq_u16(of->c[1][ch], vshrq_n_u16(hi, 8), of->inv1);
    }
    return vshrn_high_n_u16(vshrn_n_u16(lo, 8), hi, 8);
}
/* d: 16 opaque pixels as planes B, G, R, A; the alpha plane stays 255. */
static inline void opaque_fills16(uint8x16x4_t *d, const opaque_fills *of)
{
    for (int ch = 0; ch < 3; ++ch) d->val[ch] = opaque_fills_plane(d->val[ch], of, ch);
}
/* Text over a flat panel: one color over one destination color, so each
 * result depends on the mask value alone. A table of the 256 exact results
 * per color, opacity and destination, the last eight kept per thread (one
 * frame's strings share a few). */
typedef struct { uint32_t f, b; int filled; uint32_t results[256]; } flat_table;
static _Thread_local flat_table flat_tables[8];
static _Thread_local unsigned flat_next;
static const uint32_t *flat_results(uint32_t f, uint32_t b)
{
    for (int k = 0; k < 8; ++k)
        if (flat_tables[k].filled && flat_tables[k].f == f && flat_tables[k].b == b) return flat_tables[k].results;
    flat_table *t = &flat_tables[flat_next++ & 7];
    const unsigned opa = f >> 24;
    const uint32x4_t rgb = vdupq_n_u32(f & 0xffffff), under = vdupq_n_u32(b);
    for (unsigned m = 0; m < 256; m += 4) {
        uint32x4_t a = {m, m + 1, m + 2, m + 3};
        if (opa < LV_OPA_MAX) a = vshrq_n_u32(vmulq_n_u32(a, opa), 8);
        vst1q_u32(t->results + m, mix4(vorrq_u32(rgb, vshlq_n_u32(a, 24)), under));
    }
    t->f = f; t->b = b; t->filled = 1;
    return t->results;
}
static inline void fill4(uint32_t *p, const uint8_t *mask, uint32_t f, lv_opa_t opa, mix_cache *cache)
{
    uint32x4_t fg = vdupq_n_u32(f);
    if(mask) {
        uint32_t packed; memcpy(&packed, mask, 4);
        uint32x4_t a = vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(packed)))));
        if(opa < LV_OPA_MAX) a = vshrq_n_u32(vmulq_n_u32(a, opa), 8);
        fg = vorrq_u32(vdupq_n_u32(f & 0xffffff), vshlq_n_u32(a, 24));
    }
    vst1q_u32(p, cached_mix4(fg, vld1q_u32(p), cache));
}
static inline void image4(uint32_t *p, const uint32_t *src, lv_opa_t opa, mix_cache *cache)
{
    uint32x4_t f = vld1q_u32(src);
    if (opa < LV_OPA_MAX) f = vorrq_u32(vandq_u32(f, vdupq_n_u32(0xffffff)),
        vshlq_n_u32(vshrq_n_u32(vmulq_n_u32(vshrq_n_u32(f, 24), opa), 8), 24));
    vst1q_u32(p, cached_mix4(f, vld1q_u32(p), cache));
}
#endif
lv_result_t kestrel_argb_fill(lv_draw_sw_blend_fill_dsc_t *d)
{
    uint32_t f = (uint32_t)d->opa << 24 | (uint32_t)d->color.red << 16 |
                 (uint32_t)d->color.green << 8 | d->color.blue;
#ifdef __aarch64__
    mix_cache cache = {vdupq_n_u32(0), vdupq_n_u32(0), vdupq_n_u32(0)};
#endif
#ifdef __aarch64__
    opaque_fills of = {0};
    const int constant = !d->mask_buf && opaque_fills_init(&of, &f, 1);
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
                const uint32x4_t different = vorrq_u32(veorq_u32(b0,b1),
                    vorrq_u32(veorq_u32(b0,b2),veorq_u32(b0,b3)));
                if(!vmaxvq_u32(different)) {
                    const uint32x4_t result = cached_mix4(fg, b0, &cache);
                    vst1q_u32(dst+x,result);
                    vst1q_u32(dst+x+4,result);vst1q_u32(dst+x+8,result);vst1q_u32(dst+x+12,result);
                    continue;
                }
                if(vminvq_u32(vminq_u32(vminq_u32(b0, b1), vminq_u32(b2, b3))) >= 0xff000000u) {
                    uint8x16x4_t px = vld4q_u8((const uint8_t *)(dst + x));
                    if(constant) opaque_fills16(&px, &of);
                    else over_opaque16(&px, vdupq_n_u8(d->color.blue), vdupq_n_u8(d->color.green),
                                       vdupq_n_u8(d->color.red), vdupq_n_u8(d->opa));
                    vst4q_u8((uint8_t *)(dst + x), px);
                    continue;
                }
                vst1q_u32(dst+x,cached_mix4(fg,b0,&cache));
                vst1q_u32(dst+x+4,cached_mix4(fg,b1,&cache));
                vst1q_u32(dst+x+8,cached_mix4(fg,b2,&cache));
                vst1q_u32(dst+x+12,cached_mix4(fg,b3,&cache));
            }
        }
        if(mask) {
            /* Glyph strips are mostly empty. Where 16 mask values give no
             * opacity and the destination is not transparent, blending leaves
             * every pixel as it is: skip the stores and the arithmetic.
             * Strip rows are short and far apart, too short for the hardware
             * prefetcher: fetch the next row's start, and a few lines ahead. */
            if(y + 1 < d->dest_h) {
                const uint8_t *next = (const uint8_t *)d->dest_buf + (y + 1) * d->dest_stride;
                for(int k = 0; k < 4 && k * 16 < d->dest_w; ++k) __builtin_prefetch(next + k * 64, 0, 3);
            }
            for(; x + 16 <= d->dest_w; x += 16) {
                __builtin_prefetch(dst + x + 64, 0, 3);
                uint8x16_t a = vld1q_u8(mask + x);
                if(d->opa < LV_OPA_MAX)
                    a = vcombine_u8(vshrn_n_u16(vmull_u8(vget_low_u8(a), vdup_n_u8(d->opa)), 8),
                                    vshrn_n_u16(vmull_u8(vget_high_u8(a), vdup_n_u8(d->opa)), 8));
                if(vmaxvq_u8(a) <= LV_OPA_MIN) {
                    /* The smallest pixel value carries the smallest alpha. */
                    const uint32x4_t lowest = vminq_u32(vminq_u32(vld1q_u32(dst + x), vld1q_u32(dst + x + 4)),
                                                        vminq_u32(vld1q_u32(dst + x + 8), vld1q_u32(dst + x + 12)));
                    if(vminvq_u32(lowest) >> 24 > LV_OPA_MIN) continue;
                }
                if(opaque_at(dst + x)) {
                    uint8x16x4_t px = vld4q_u8((const uint8_t *)(dst + x));
                    over_opaque16(&px, vdupq_n_u8(d->color.blue), vdupq_n_u8(d->color.green), vdupq_n_u8(d->color.red), a);
                    vst4q_u8((uint8_t *)(dst + x), px);
                    continue;
                }
                const uint32x4_t b0 = vld1q_u32(dst + x);
                const uint32x4_t different = vorrq_u32(veorq_u32(b0, vld1q_u32(dst + x + 4)),
                    vorrq_u32(veorq_u32(b0, vld1q_u32(dst + x + 8)), veorq_u32(b0, vld1q_u32(dst + x + 12))));
                if(!vmaxvq_u32(different)) {
                    /* Text over a flat translucent panel (the menu blade). */
                    const uint32_t *results = flat_results(f, vgetq_lane_u32(b0, 0));
                    const uint8_t *m = mask + x;
                    for(int k = 0; k < 16; ++k) dst[x + k] = results[m[k]];
                    continue;
                }
                for(int k = 0; k < 16; k += 4) fill4(dst + x + k, mask + x + k, f, d->opa, &cache);
            }
        }
        for (; x + 4 <= d->dest_w; x += 4) fill4(dst + x, mask ? mask + x : NULL, f, d->opa, &cache);
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
        /* Image rows (HUD panels, labels) are short: fetch ahead, as above. */
        if (y + 1 < d->dest_h) {
            const uint8_t *next = (const uint8_t *)d->dest_buf + (y + 1) * d->dest_stride;
            const uint8_t *next_src = (const uint8_t *)d->src_buf + (y + 1) * d->src_stride;
            for (int k = 0; k < 4 && k * 16 < d->dest_w; ++k) { __builtin_prefetch(next + k * 64, 0, 3); __builtin_prefetch(next_src + k * 64, 0, 3); }
        }
        for (; x + 16 <= d->dest_w; x += 16) {
            __builtin_prefetch(dst + x + 64, 0, 3); __builtin_prefetch(src + x + 64, 0, 3);
            if(d->opa >= LV_OPA_MAX) {
                /* Source pixels at LV_OPA_MAX or more replace the destination
                 * (the idle artwork is opaque): a copy, the destination unread. */
                const uint32x4_t s0 = vld1q_u32(src + x), s1 = vld1q_u32(src + x + 4);
                const uint32x4_t s2 = vld1q_u32(src + x + 8), s3 = vld1q_u32(src + x + 12);
                if(vminvq_u32(vminq_u32(vminq_u32(s0, s1), vminq_u32(s2, s3))) >= (uint32_t)LV_OPA_MAX << 24) {
                    vst1q_u32(dst + x, s0); vst1q_u32(dst + x + 4, s1);
                    vst1q_u32(dst + x + 8, s2); vst1q_u32(dst + x + 12, s3);
                    continue;
                }
            }
            if(!opaque_at(dst + x)) {
                for(int k = 0; k < 16; k += 4) image4(dst + x + k, src + x + k, d->opa, &cache);
                continue;
            }
            uint8x16x4_t px = vld4q_u8((const uint8_t *)(dst + x));
            const uint8x16x4_t f = vld4q_u8((const uint8_t *)(src + x));
            uint8x16_t a = f.val[3];
            if(d->opa < LV_OPA_MAX)
                a = vcombine_u8(vshrn_n_u16(vmull_u8(vget_low_u8(a), vdup_n_u8(d->opa)), 8),
                                vshrn_n_u16(vmull_u8(vget_high_u8(a), vdup_n_u8(d->opa)), 8));
            over_opaque16(&px, f.val[0], f.val[1], f.val[2], a);
            vst4q_u8((uint8_t *)(dst + x), px);
        }
        for (; x + 4 <= d->dest_w; x += 4) image4(dst + x, src + x, d->opa, &cache);
#endif
        for (; x < d->dest_w; ++x) {
            uint32_t f = src[x];
            if (d->opa < LV_OPA_MAX) f = (f & 0xffffff) | ((((f >> 24) * d->opa) >> 8) << 24);
            dst[x] = mix(f, dst[x]);
        }
    }
    return LV_RESULT_OK;
}
void kestrel_argb_fills(uint32_t *dst, int dst_stride_px, const uint32_t *src, int src_stride_px,
                        int w, int h, int fills, uint32_t under, uint32_t over)
{
    const int two = fills == 2;
#ifdef __aarch64__
    const uint32x4_t u = vdupq_n_u32(under), o = vdupq_n_u32(over);
    mix_cache first = {vdupq_n_u32(0), vdupq_n_u32(0), vdupq_n_u32(0)}, second = first;
    const uint32_t colors[2] = {under, over};
    opaque_fills of = {0};
    const int constant = opaque_fills_init(&of, colors, two ? 2 : 1);
#endif
    for (int y = 0; y < h; ++y, dst += dst_stride_px, src += src_stride_px) {
        int x = 0;
#ifdef __aarch64__
        for (; x + 16 <= w; x += 16) {
            const uint32x4_t b0 = vld1q_u32(src + x), b1 = vld1q_u32(src + x + 4);
            const uint32x4_t b2 = vld1q_u32(src + x + 8), b3 = vld1q_u32(src + x + 12);
            const uint32x4_t different = vorrq_u32(veorq_u32(b0, b1), vorrq_u32(veorq_u32(b0, b2), veorq_u32(b0, b3)));
            if (!vmaxvq_u32(different)) {
                uint32x4_t r = cached_mix4(u, b0, &first);
                if (two) r = cached_mix4(o, r, &second);
                vst1q_u32(dst + x, r); vst1q_u32(dst + x + 4, r); vst1q_u32(dst + x + 8, r); vst1q_u32(dst + x + 12, r);
                continue;
            }
            if (constant && vminvq_u32(vminq_u32(vminq_u32(b0, b1), vminq_u32(b2, b3))) >= 0xff000000u) {
                uint8x16x4_t px = vld4q_u8((const uint8_t *)(src + x));
                opaque_fills16(&px, &of);
                vst4q_u8((uint8_t *)(dst + x), px);
                continue;
            }
            uint32x4_t r0 = cached_mix4(u, b0, &first), r1 = cached_mix4(u, b1, &first);
            uint32x4_t r2 = cached_mix4(u, b2, &first), r3 = cached_mix4(u, b3, &first);
            if (two) {
                r0 = cached_mix4(o, r0, &second); r1 = cached_mix4(o, r1, &second);
                r2 = cached_mix4(o, r2, &second); r3 = cached_mix4(o, r3, &second);
            }
            vst1q_u32(dst + x, r0); vst1q_u32(dst + x + 4, r1); vst1q_u32(dst + x + 8, r2); vst1q_u32(dst + x + 12, r3);
        }
#endif
        for (; x < w; ++x) { const uint32_t p = mix(under, src[x]); dst[x] = two ? mix(over, p) : p; }
    }
}
void kestrel_argb_fill2(uint32_t *dst, int w, int h, int stride_px, uint32_t under, uint32_t over)
{
    kestrel_argb_fills(dst, stride_px, dst, stride_px, w, h, 2, under, over);
}

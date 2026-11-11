#pragma once
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#ifdef __cplusplus
extern "C" {
#endif
lv_result_t kestrel_argb_fill(lv_draw_sw_blend_fill_dsc_t *d);
lv_result_t kestrel_argb_image(lv_draw_sw_blend_image_dsc_t *d);
/* One pixel exactly as the fills and images above blend it: f over b. */
uint32_t kestrel_argb_mix(uint32_t f, uint32_t b);
/* In place, exactly as two unmasked fills would: `under` over each pixel,
 * then `over` over that. One pass over memory instead of two. */
void kestrel_argb_fill2(uint32_t *dst, int w, int h, int stride_px, uint32_t under, uint32_t over);
/* The same over the pixels of src, stored to dst (src may be dst): `under`
 * then, when fills is 2, `over`. Reads src once and never reads dst. */
void kestrel_argb_fills(uint32_t *dst, int dst_stride_px, const uint32_t *src, int src_stride_px,
                        int w, int h, int fills, uint32_t under, uint32_t over);
#ifdef __cplusplus
}
#endif
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_WITH_OPA(d) kestrel_argb_fill(d)
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_WITH_MASK(d) kestrel_argb_fill(d)
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_MIX_MASK_OPA(d) kestrel_argb_fill(d)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_ARGB8888(d) kestrel_argb_image(d)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_ARGB8888_WITH_OPA(d) kestrel_argb_image(d)

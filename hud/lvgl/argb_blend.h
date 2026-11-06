#pragma once
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#ifdef __cplusplus
extern "C" {
#endif
lv_result_t kestrel_argb_fill(lv_draw_sw_blend_fill_dsc_t *d);
lv_result_t kestrel_argb_image(lv_draw_sw_blend_image_dsc_t *d);
#ifdef __cplusplus
}
#endif
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_WITH_OPA(d) kestrel_argb_fill(d)
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_WITH_MASK(d) kestrel_argb_fill(d)
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_MIX_MASK_OPA(d) kestrel_argb_fill(d)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_ARGB8888(d) kestrel_argb_image(d)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_ARGB8888_WITH_OPA(d) kestrel_argb_image(d)

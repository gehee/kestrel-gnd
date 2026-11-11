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
        std::vector<uint8_t> masks(stride*height+2);
        for(size_t i=0;i<masks.size();++i)masks[i]=uint8_t(i/256);
        for(int alpha:{0,1,2,3,127,252,253,254,255}) {
            dst=bg;
            lv_draw_sw_blend_fill_dsc_t d{};
            d.dest_buf=dst.data()+1;d.dest_w=width;d.dest_h=height;d.dest_stride=stride*4;
            d.color=lv_color_make(19,77,230);d.opa=alpha;d.mask_buf=masks.data()+1;d.mask_stride=stride;
            check(kestrel_argb_fill(&d)==LV_RESULT_OK);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                const unsigned a=alpha>=LV_OPA_MAX?masks[at]:(masks[at]*alpha)>>8;
                check(dst[at]==(x<width?reference((a<<24)|0x134de6,bg[at]):bg[at]));
            }
        }
        // Glyph-like masks: long empty runs (skipped where the destination is
        // not transparent) broken by short strokes, over destinations that mix
        // transparent and translucent pixels.
        for(int alpha:{1,2,3,127,252,255}) {
            for(size_t i=0;i<masks.size();++i) {
                rng=rng*1664525+1013904223;
                masks[i]=(rng>>24)<16?uint8_t(rng>>8):(rng>>24)<24?uint8_t((rng>>16)&3):0;
                rng=rng*1664525+1013904223;
                bg[i]=((rng>>26)==0?uint32_t((rng>>20)&3):0x30u+((rng>>20)&0x7f))<<24|(rng&0xffffff);
            }
            dst=bg;
            lv_draw_sw_blend_fill_dsc_t d{};
            d.dest_buf=dst.data()+1;d.dest_w=width;d.dest_h=height;d.dest_stride=stride*4;
            d.color=lv_color_make(240,180,30);d.opa=alpha;d.mask_buf=masks.data()+1;d.mask_stride=stride;
            check(kestrel_argb_fill(&d)==LV_RESULT_OK);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                const unsigned a=alpha>=LV_OPA_MAX?masks[at]:(masks[at]*alpha)>>8;
                check(dst[at]==(x<width?reference((a<<24)|0xf0b41e,bg[at]):bg[at]));
            }
        }
        // Text over flat panels: runs of one destination color (every alpha
        // branch, three colors taking turns), full and glyph-like masks.
        {
            const uint32_t flats[]={0x00123456,0x01654321,0x02aabbcc,0x03ccbbaa,0x19203040,0x7f405060,0xf2112233,
                                    0xfc332211,0xfd445566,0xfe665544,0xff778899};
            for(int sparse:{0,1}) {
                for(size_t i=0,run=0,kind=0;i<bg.size();++i) {
                    rng=rng*1664525+1013904223;
                    if(i%48==0) {kind=rng>>28;run=(run+1)%3;}
                    bg[i]=kind<11?flats[(kind+run*4)%11]:(rng|((rng>>9)&1?0xff000000u:0));
                    rng=rng*1664525+1013904223;
                    masks[i]=sparse?((rng>>24)<12?uint8_t(rng>>8):(rng>>24)<20?uint8_t((rng>>16)&3):0):uint8_t(rng>>8);
                }
                for(int alpha:{1,2,3,127,252,253,255}) {
                    dst=bg;
                    lv_draw_sw_blend_fill_dsc_t d{};
                    d.dest_buf=dst.data()+1;d.dest_w=width;d.dest_h=height;d.dest_stride=stride*4;
                    d.color=lv_color_make(222,40,7);d.opa=alpha;d.mask_buf=masks.data()+1;d.mask_stride=stride;
                    check(kestrel_argb_fill(&d)==LV_RESULT_OK);
                    for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                        const auto at=1+y*stride+x;
                        const unsigned a=alpha>=LV_OPA_MAX?masks[at]:(masks[at]*alpha)>>8;
                        check(dst[at]==(x<width?reference((a<<24)|0xde2807,bg[at]):bg[at]));
                    }
                }
            }
        }
        for(int f=0;f<256;++f) for(int b=0;b<256;++b) {
            const uint32_t fg=uint32_t(f)<<24|0x13ad5e, bk=uint32_t(b)<<24|0xc0ffee;
            check(kestrel_argb_mix(fg,bk)==reference(fg,bk));
        }
        // Opaque, varied destinations (the idle artwork) take a byte-plane
        // path; an occasional translucent pixel sends its block back to the
        // general one. Fills, masked fills and images, every opacity branch.
        for(size_t i=0;i<bg.size();++i) {
            rng=rng*1664525+1013904223;
            bg[i]=((rng>>25)==0?uint32_t(rng>>8)&0xff:0xffu)<<24|(rng&0xffffff);
            rng=rng*1664525+1013904223;
            src[i]=rng;
            masks[i]=uint8_t(rng>>8);
        }
        for(int alpha:{0,1,2,3,127,252,253,254,255}) {
            dst=bg;
            lv_draw_sw_blend_fill_dsc_t fill{};
            fill.dest_buf=dst.data()+1;fill.dest_w=width;fill.dest_h=height;fill.dest_stride=stride*4;
            fill.color=lv_color_make(19,77,230);fill.opa=alpha;
            check(kestrel_argb_fill(&fill)==LV_RESULT_OK);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                check(dst[at]==(x<width?reference((uint32_t(alpha)<<24)|0x134de6,bg[at]):bg[at]));
            }
            dst=bg;
            fill.mask_buf=masks.data()+1;fill.mask_stride=stride;
            check(kestrel_argb_fill(&fill)==LV_RESULT_OK);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                const unsigned a=alpha>=LV_OPA_MAX?masks[at]:(masks[at]*alpha)>>8;
                check(dst[at]==(x<width?reference((a<<24)|0x134de6,bg[at]):bg[at]));
            }
            dst=bg;
            lv_draw_sw_blend_image_dsc_t image{};
            image.src_buf=src.data()+1;image.dest_buf=dst.data()+1;image.dest_w=width;image.dest_h=height;
            image.src_stride=image.dest_stride=stride*4;image.opa=alpha;image.blend_mode=LV_BLEND_MODE_NORMAL;
            check(kestrel_argb_image(&image)==LV_RESULT_OK);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                uint32_t f=src[at];
                if(alpha<LV_OPA_MAX) f=(f&0xffffff)|((((f>>24)*alpha)>>8)<<24);
                check(dst[at]==(x<width?reference(f,bg[at]):bg[at]));
            }
        }
        // Two fills in one pass: opaque, translucent, transparent and uniform
        // destinations, every pair of threshold opacities.
        for(size_t i=0,kind=0;i<bg.size();++i) {
            rng=rng*1664525+1013904223;
            if(i%40==0) kind=rng>>29;  // runs of one kind, so whole blocks take each path
            bg[i]=kind<3?(0xffu<<24|(rng&0xffffff)):kind<5?0x28456789:kind<6?0:(((rng>>8)&0xff)<<24|(rng&0xffffff));
        }
        for(int under:{0,1,2,3,25,41,127,252,253,255}) for(int over:{0,2,3,127,239,252,253,255}) {
            const uint32_t u=uint32_t(under)<<24|0x0d1a2b, o=uint32_t(over)<<24|0x0d1a2b;
            dst=bg;
            kestrel_argb_fill2(dst.data()+1,width,height,stride,u,o);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                check(dst[at]==(x<width?reference(o,reference(u,bg[at])):bg[at]));
            }
        }
        // The same fills read from another buffer: the destination's own
        // pixels never count, one or two fills, different strides.
        {
            const int sstride=stride+3;
            std::vector<uint32_t> from(size_t(sstride)*height+2);
            for(size_t i=0,kind=0;i<from.size();++i) {
                rng=rng*1664525+1013904223;
                if(i%40==0) kind=rng>>29;
                from[i]=kind<3?(0xffu<<24|(rng&0xffffff)):kind<5?0x28456789:kind<6?0:(((rng>>8)&0xff)<<24|(rng&0xffffff));
            }
            const auto kept=from;
            for(int fills:{1,2}) for(int under:{0,2,3,25,41,127,252,253,255}) for(int over:{0,3,127,240,253}) {
                const uint32_t u=uint32_t(under)<<24|0x0d1a2b, o=uint32_t(over)<<24|0x3c2d1e;
                std::fill(dst.begin(),dst.end(),0x5a5a5a5au);
                kestrel_argb_fills(dst.data()+1,stride,from.data()+2,sstride,width,height,fills,u,o);
                for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                    const auto at=1+y*stride+x;
                    const uint32_t s=from[2+y*sstride+x], once=reference(u,s);
                    check(dst[at]==(x<width?(fills==2?reference(o,once):once):0x5a5a5a5au));
                }
                check(dst.front()==0x5a5a5a5au && dst.back()==0x5a5a5a5au && from==kept);
                if(fills==1) break;  // `over` is unused
            }
        }
        // Images that are mostly opaque (the idle artwork): whole blocks at
        // LV_OPA_MAX or more are copied, others blend; every opacity.
        for(size_t i=0,kind=0;i<src.size();++i) {
            rng=rng*1664525+1013904223;
            if(i%40==0) kind=rng>>29;
            const uint32_t a=kind<4?0xff:kind==4?LV_OPA_MAX:kind==5?LV_OPA_MAX-1:(rng>>8)&0xff;
            src[i]=a<<24|(rng&0xffffff);
            rng=rng*1664525+1013904223;
            bg[i]=((rng>>28)<4?0xffu:(rng>>24)&0xff)<<24|(rng&0xffffff);
        }
        for(int opacity:{0,2,3,127,252,253,254,255}) {
            dst=bg;
            lv_draw_sw_blend_image_dsc_t image{};
            image.src_buf=src.data()+1;image.dest_buf=dst.data()+1;image.dest_w=width;image.dest_h=height;
            image.src_stride=image.dest_stride=stride*4;image.opa=opacity;image.blend_mode=LV_BLEND_MODE_NORMAL;
            check(kestrel_argb_image(&image)==LV_RESULT_OK);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                uint32_t f=src[at];
                if(opacity<LV_OPA_MAX) f=(f&0xffffff)|((((f>>24)*opacity)>>8)<<24);
                check(dst[at]==(x<width?reference(f,bg[at]):bg[at]));
            }
        }
        // Exercise repeated block hits and transitions out of the cache.
        std::fill(src.begin(),src.end(),0xe0123456);std::fill(bg.begin(),bg.end(),0x28456789);
        for(size_t i=7;i<src.size();i+=113)src[i]^=0x1a90375b;
        dst=bg;lv_draw_sw_blend_image_dsc_t d{};
        d.src_buf=src.data()+1;d.dest_buf=dst.data()+1;d.dest_w=width;d.dest_h=height;
        d.src_stride=d.dest_stride=stride*4;d.opa=255;d.blend_mode=LV_BLEND_MODE_NORMAL;
        check(kestrel_argb_image(&d)==LV_RESULT_OK);
        for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
            const auto at=1+y*stride+x;check(dst[at]==(x<width?reference(src[at],bg[at]):bg[at]));
        }
        for(size_t i=7;i<bg.size();i+=113)bg[i]^=0xe190375b;
        for(int opacity:{0,1,2,3,127,252,253,254,255}) {
            dst=bg;lv_draw_sw_blend_fill_dsc_t fill{};
            fill.dest_buf=dst.data()+1;fill.dest_w=width;fill.dest_h=height;fill.dest_stride=stride*4;
            fill.color=lv_color_make(19,77,230);fill.opa=opacity;
            check(kestrel_argb_fill(&fill)==LV_RESULT_OK);
            for(int y=0;y<height;++y)for(int x=0;x<stride;++x) {
                const auto at=1+y*stride+x;
                check(dst[at]==(x<width?reference((uint32_t(opacity)<<24)|0x134de6,bg[at]):bg[at]));
            }
        }
        puts("ARGB blend matches LVGL across alpha pairs, masks (sparse ones too, over flat panels), opaque sources and destinations, double fills from any buffer, opacity thresholds, strides, tails and cached spans");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}

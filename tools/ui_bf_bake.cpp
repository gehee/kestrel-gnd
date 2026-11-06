// Build-host rasterization of the same vector glyphs used by VRX Pro.
#include "msp/betaflight_glyphs.hpp"
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>
static void word(std::ostream& out,uint32_t v) {char b[]={char(v),char(v>>8),char(v>>16),char(v>>24)};out.write(b,4);}
int main(int argc,char** argv) {
 try {
    if(argc!=3) throw std::runtime_error("usage: ui-bf-bake FONT OUTPUT");
    int count=0;FcPattern* pattern=FcFreeTypeQuery(reinterpret_cast<const FcChar8*>(argv[1]),0,nullptr,&count);
    if(!pattern)throw std::runtime_error("Cannot read source font");
    auto* face=cairo_ft_font_face_create_for_pattern(pattern);FcPatternDestroy(pattern);
    constexpr int w=36,h=54;
    std::ofstream out(argv[2],std::ios::binary);out.write("KBF1",4);word(out,w);word(out,h);word(out,512);
    for(int glyph=0;glyph<512;++glyph) {
        auto* surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);auto* cr=cairo_create(surface);
        cairo_set_source_rgb(cr,1,1,1);cairo_set_font_face(cr,face);cairo_set_font_size(cr,40);
        if(!BetaflightGlyphs::draw_glyph(cr,glyph,w,h) && glyph>=32 && glyph<=95) {
            char s[]={char(glyph),0};cairo_text_extents_t e{};cairo_text_extents(cr,s,&e);
            cairo_move_to(cr,(w-e.width)/2-e.x_bearing,(h-e.height)/2-e.y_bearing);cairo_show_text(cr,s);
        }
        cairo_surface_flush(surface);const auto* pixels=reinterpret_cast<const uint32_t*>(cairo_image_surface_get_data(surface));
        const int stride=cairo_image_surface_get_stride(surface)/4;
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            const uint32_t color=pixels[y*stride+x];unsigned alpha=color>>24;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
                if(x+dx>=0 && x+dx<w && y+dy>=0 && y+dy<h) alpha=std::max(alpha,pixels[(y+dy)*stride+x+dx]>>24);
            uint32_t straight=alpha<<24;
            if(alpha) for(int shift:{0,8,16}) straight|=std::min(255u,((color>>shift)&255)*255/alpha)<<shift;
            word(out,straight);
        }
        cairo_destroy(cr);cairo_surface_destroy(surface);
    }
    cairo_font_face_destroy(face);if(!out)throw std::runtime_error("Writing glyph atlas failed");return 0;
 } catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}

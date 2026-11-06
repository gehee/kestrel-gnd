#include "betaflight.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>
namespace kestrel_ui {
BetaflightAtlas::BetaflightAtlas(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)return;
    uint8_t header[16]{};
    auto word=[&](int at){return unsigned(header[at])|unsigned(header[at+1])<<8|unsigned(header[at+2])<<16|unsigned(header[at+3])<<24;};
    if(!in.read(reinterpret_cast<char*>(header),16) || std::memcmp(header,"KBF1",4) || word(12)!=512 ||
       !((word(4)==36 && word(8)==54) || (word(4)==24 && word(8)==36)))
        throw std::runtime_error("Invalid Betaflight glyph atlas");
    glyph_width_=int(word(4));glyph_height_=int(word(8));
    pixels_.resize(512*glyph_width_*glyph_height_);
    if(!in.read(reinterpret_cast<char*>(pixels_.data()),pixels_.size()*4) || in.peek()!=std::char_traits<char>::eof())
        throw std::runtime_error("Truncated Betaflight glyph atlas");
    for(size_t i=0;i<glyphs_.size();++i) {
        auto& g=glyphs_[i];g.header.magic=LV_IMAGE_HEADER_MAGIC;g.header.cf=LV_COLOR_FORMAT_ARGB8888;
        g.header.w=glyph_width_;g.header.h=glyph_height_;g.header.stride=glyph_width_*4;
        g.data_size=glyph_width_*glyph_height_*4;g.data=reinterpret_cast<const uint8_t*>(pixels_.data()+i*glyph_width_*glyph_height_);
    }
}
void BetaflightAtlas::draw(lv_layer_t* layer,const std::array<uint16_t,1060>& grid,int width,int height) const {
    if(!available())return;
    for(int row=0;row<20;++row) for(int col=0;col<53;++col) {
        const unsigned glyph=grid[row*53+col];if(!glyph || glyph==32 || glyph>=glyphs_.size())continue;
        lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.src=&glyphs_[glyph];
        d.scale_x=width*36*256/(1920*glyph_width_);d.scale_y=height*54*256/(1080*glyph_height_);d.pivot={0,0};
        const int x=(6+col*36)*width/1920,y=row*54*height/1080;
        lv_area_t area{x,y,x+glyph_width_-1,y+glyph_height_-1};lv_draw_image(layer,&d,&area);
    }
}
}

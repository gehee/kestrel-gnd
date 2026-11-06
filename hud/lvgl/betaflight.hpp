#pragma once
#include "lvgl.h"
#include <array>
#include <string>
#include <vector>
namespace kestrel_ui {
class BetaflightAtlas {
public:
    explicit BetaflightAtlas(const std::string& path);
    bool available() const {return !pixels_.empty();}
    void draw(lv_layer_t* layer,const std::array<uint16_t,1060>& grid,int width,int height) const;
private:
    std::vector<uint32_t> pixels_;
    std::array<lv_image_dsc_t,512> glyphs_{};
};
}

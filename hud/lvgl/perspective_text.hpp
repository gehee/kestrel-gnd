#pragma once
#include "font_atlas.hpp"
#include "lvgl.h"
#include <map>
#include <tuple>

namespace kestrel_ui {
class PerspectiveText {
public:
    PerspectiveText(const std::string& font_path, int width, int height, size_t budget = 1024 * 1024);
    ~PerspectiveText();
    void begin_frame();
    float advance(const std::string& text) const { return font_.advance(text); }
    float design_advance(const std::string& text, float size) const;
    void panel(lv_layer_t* layer, const std::string& text, bool left, float x,
               float y, float size, bool right_align, const float* rgb, float alpha);
    void canvas(lv_layer_t* layer, const std::string& text, float x, float baseline,
                float pixels, bool right_align, const float* rgb, float alpha,
                int offset_x = 0, int offset_y = 0);
    size_t cached_bytes() const { return bytes_; }
    uint64_t rasterizations() const { return rasterizations_; }
private:
    using Key = std::tuple<std::string, std::array<float, 8>>;
    struct Cached {
        canopy::AlphaSurface surface;
        lv_image_dsc_t image{};
        uint64_t used = 0;
    };
    FontAtlas font_;
    int width_, height_;
    size_t bytes_ = 0;
    size_t budget_;
    uint64_t clock_ = 0, rasterizations_ = 0;
    std::map<Key, Cached> cache_;
    void paint(lv_layer_t* layer, const std::string& text, const std::array<canopy::Point, 4>& quad,
               const float* rgb, float alpha, int offset_x = 0, int offset_y = 0);
    void remove(std::map<Key, Cached>::iterator entry);
};
} // namespace kestrel_ui

#pragma once
#include "font_atlas.hpp"
#include "lvgl.h"
#include <map>
#include <memory>
#include <tuple>

namespace kestrel_ui {
class PerspectiveText {
public:
    PerspectiveText(const std::string& font_path, int width, int height, size_t budget = 1024 * 1024);
    ~PerspectiveText();
    void begin_frame();
    float advance(const std::string& text) const { return font_.advance(text)*scale_; }
    void set_scale(float scale) { scale_=scale; }
    float canvas_advance(const std::string& text, float pixels) const;
    float design_advance(const std::string& text, float size) const;
    void panel(lv_layer_t* layer, const std::string& text, bool left, float x,
               float y, float size, bool right_align, const float* rgb, float alpha);
    void canvas(lv_layer_t* layer, const std::string& text, float x, float baseline,
                float pixels, bool right_align, const float* rgb, float alpha,
                int offset_x = 0, int offset_y = 0);
    // While set, every painted text's bounds are added to *area (x1 > x2: empty).
    void collect_bounds(lv_area_t* area) { bounds_ = area; }
    size_t cached_bytes() const { return bytes_; }
    uint64_t rasterizations() const { return rasterizations_; }
private:
    using Key = std::tuple<std::string, std::array<float, 8>, int>;
    struct Cached {
        canopy::AlphaSurface surface;
        lv_image_dsc_t image{};
        uint64_t used = 0;
    };
    const FontAtlas* native_font(float pixels) const;
    std::string bank_;
    mutable std::map<int,std::unique_ptr<FontAtlas>> native_fonts_;
    FontAtlas font_;
    int width_, height_;
    float scale_=1;
    size_t bytes_ = 0;
    size_t budget_;
    lv_area_t* bounds_ = nullptr;
    uint64_t clock_ = 0, rasterizations_ = 0;
    std::map<Key, Cached> cache_;
    void paint(lv_layer_t* layer, const std::string& text, const std::array<canopy::Point, 4>& quad,
               const float* rgb, float alpha, int offset_x = 0, int offset_y = 0, const FontAtlas* native = nullptr);
    void remove(std::map<Key, Cached>::iterator entry);
};
} // namespace kestrel_ui

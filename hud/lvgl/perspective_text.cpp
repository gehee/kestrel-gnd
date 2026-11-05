#include "perspective_text.hpp"

namespace kestrel_ui {
PerspectiveText::PerspectiveText(const std::string& path, int w, int h, size_t budget) : font_(path), width_(w), height_(h), budget_(budget) {}
PerspectiveText::~PerspectiveText() { while (!cache_.empty()) remove(cache_.begin()); }
void PerspectiveText::remove(std::map<Key, Cached>::iterator entry) {
    lv_image_cache_drop(&entry->second.image);
    lv_image_header_cache_drop(&entry->second.image);
    bytes_ -= entry->second.surface.pixels.size();
    cache_.erase(entry);
}
void PerspectiveText::begin_frame() {
    // Called before lv_timer_handler, when the previous synchronous software
    // draw is complete. Never evict images from a DRAW callback: LVGL may still
    // have queued draw tasks referring to them in another partial-buffer strip.
    while (cache_.size() > 96 || bytes_ > budget_) {
        auto victim = std::min_element(cache_.begin(), cache_.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        remove(victim);
    }
}
float PerspectiveText::design_advance(const std::string& text, float size) const {
    return font_.advance(text) * size / (canopy::kTextCellHeight * canopy::screen_frame(width_, height_).UX);
}
void PerspectiveText::panel(lv_layer_t* layer, const std::string& text, bool left, float x,
                            float y, float size, bool right_align, const float* rgb, float alpha) {
    if (text.empty() || alpha <= 0) return;
    // The glyph strip has the same padding/aligned width as the GLES texture.
    const float bitmap_width = float((int(std::ceil(font_.advance(text) + 6)) + 15) & ~15);
    const auto quad = canopy::text_quad(width_, height_, left, x, y, size, bitmap_width, font_.advance(text), right_align);
    paint(layer, text, quad, rgb, alpha);
}
void PerspectiveText::canvas(lv_layer_t* layer, const std::string& text, float x, float baseline,
                             float pixels, bool right_align, const float* rgb, float alpha,
                             int offset_x, int offset_y) {
    if (text.empty() || alpha <= 0) return;
    const float scale = pixels / canopy::kFontPixels;
    if (right_align) x -= font_.advance(text) * scale;
    const float top = baseline - canopy::kTextBaseline * scale;
    const float h = canopy::kTextCellHeight * scale;
    const float w = ((int(std::ceil(font_.advance(text) + 6)) + 15) & ~15) * scale;
    // Menu translation does not change the rasterization key: cached glyphs
    // slide as one object, rather than generating a new bitmap each tick.
    paint(layer, text, {{{x, top}, {x + w, top}, {x + w, top + h}, {x, top + h}}}, rgb, alpha, offset_x, offset_y);
}
void PerspectiveText::paint(lv_layer_t* layer, const std::string& text, const std::array<canopy::Point, 4>& quad,
                            const float* rgb, float alpha, int offset_x, int offset_y) {
    std::array<float, 8> coordinates;
    for (size_t i = 0; i < quad.size(); ++i) { coordinates[i * 2] = quad[i].x; coordinates[i * 2 + 1] = quad[i].y; }
    Key key{text, coordinates};
    auto found = cache_.find(key);
    if (found == cache_.end()) {
        Cached entry;
        entry.surface = canopy::warp_alpha(font_.rasterize(text), quad, width_, height_);
        ++rasterizations_;
        if (!entry.surface.width || !entry.surface.height) return;
        bytes_ += entry.surface.pixels.size();
        found = cache_.emplace(std::move(key), std::move(entry)).first;
        auto& c = found->second;
        c.image.header.magic = LV_IMAGE_HEADER_MAGIC;
        c.image.header.cf = LV_COLOR_FORMAT_A8;
        c.image.header.w = c.surface.width; c.image.header.h = c.surface.height;
        c.image.header.stride = c.surface.width;
        c.image.data_size = c.surface.pixels.size(); c.image.data = c.surface.pixels.data();
    }
    auto& c = found->second;
    c.used = ++clock_;
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = &c.image;
    image.recolor = lv_color_make(uint8_t(rgb[0] * 255), uint8_t(rgb[1] * 255), uint8_t(rgb[2] * 255));
    image.recolor_opa = LV_OPA_COVER;
    image.opa = lv_opa_t(std::clamp(alpha * 255, 0.0f, 255.0f));
    const int x = c.surface.x + offset_x, y = c.surface.y + offset_y;
    lv_area_t area{x, y, x + c.surface.width - 1, y + c.surface.height - 1};
    lv_draw_image(layer, &image, &area);
}
} // namespace kestrel_ui

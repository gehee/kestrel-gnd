#include "idle_scene.hpp"
#include "hud/hud_theme.hpp"
#include <fstream>
#include <cstring>
#include <stdexcept>

namespace kestrel_ui {
IdleScene::IdleScene(const std::string& assets, int w, int h)
    : path_(assets + "/idle-scenes.kui"), width_(w), height_(h),
      text_(assets + "/canopy-font.kuf", w, h, 512 * 1024) {}
IdleScene::~IdleScene() { drop_image(); }
void IdleScene::drop_image() {
    lv_image_cache_drop(&image_); lv_image_header_cache_drop(&image_);
    pixels_.clear(); image_ = {};
}
void IdleScene::prepare(const Model& model, uint64_t now) {
    text_.begin_frame();
    if (!model.hud_visible() || !model.show_status(now) || model.backend.vrx.link_valid(now)) return;
    const std::string theme = hud_theme_at(model.theme).name;
    if (loaded_theme_ == theme) return;
    // Replace only between synchronous LVGL refreshes, never in a draw callback.
    drop_image();
    std::ifstream file(path_, std::ios::binary);
    if (!file) { loaded_theme_ = theme; return; } // Minimal installs show a text wordmark.
    uint8_t header[20]{};
    auto word = [&](int offset) { return uint32_t(header[offset]) | uint32_t(header[offset + 1]) << 8 |
        uint32_t(header[offset + 2]) << 16 | uint32_t(header[offset + 3]) << 24; };
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "KUIB", 4) ||
        (word(4) != 1 && word(4) != 2) || word(8) != unsigned(width_) || word(12) != unsigned(height_) ||
        width_ < 1 || width_ > 4096 || height_ < 1 || height_ > 2160 || !word(16) || word(16) > 64)
        throw std::runtime_error("Invalid static idle artwork: " + path_);
    const uint64_t bytes = uint64_t(width_) * height_ * 4;
    const uint64_t length = 20 + word(16) * (32 + bytes);
    file.seekg(0, std::ios::end);
    if (file.tellg() != std::streamoff(length)) throw std::runtime_error("Invalid static idle artwork length: " + path_);
    file.seekg(20);
    for (uint32_t i = 0; i < word(16); ++i) {
        char name[32]{};
        if (!file.read(name, sizeof(name)) || !std::memchr(name, 0, sizeof(name)))
            throw std::runtime_error("Invalid static idle theme: " + path_);
        if (theme == name) {
            pixels_.resize(bytes);
            if (!file.read(reinterpret_cast<char*>(pixels_.data()), pixels_.size()))
                throw std::runtime_error("Cannot read static idle artwork: " + path_);
            if (word(4) == 2 && i != 0) {
                std::vector<uint8_t> base(bytes);
                file.seekg(52); // Header, then the first theme's name.
                if (!file.read(reinterpret_cast<char*>(base.data()),base.size()))
                    throw std::runtime_error("Truncated base idle artwork: " + path_);
                for (size_t j=0;j<pixels_.size();++j) pixels_[j] ^= base[j];
            }
            image_.header.magic = LV_IMAGE_HEADER_MAGIC;
            image_.header.cf = LV_COLOR_FORMAT_ARGB8888;
            image_.header.w = width_; image_.header.h = height_; image_.header.stride = width_ * 4;
            image_.data_size = pixels_.size(); image_.data = pixels_.data();
            loaded_theme_ = theme;
            return;
        }
        file.seekg(bytes, std::ios::cur);
    }
    throw std::runtime_error("Static idle theme missing; rebuild assets: " + theme);
}
void IdleScene::draw(lv_layer_t* layer, const Model& model, uint64_t now) {
    if (!model.hud_visible() || !model.show_status(now)) return;
    const auto& theme = hud_theme_at(model.theme);
    if (model.backend.vrx.link_valid(now)) {
        // A radio link is not a decoded picture. Keep acquisition status live.
        if (!model.menu_open) {
            const std::string title = "ACQUIRING VIDEO", status = "WAITING FOR VIDEO";
            const float size = 34 * height_ / 1080.0f;
            text_.canvas(layer, title, (width_ - text_.canvas_advance(title,size)) / 2,
                         height_ * 0.48f, size, false, theme.text, 1);
            text_.canvas(layer, status, (width_ - text_.canvas_advance(status,size*.5f)) / 2,
                         height_ * 0.53f, size * 0.5f, false, theme.accent, 1);
        }
        return;
    }
    if (!pixels_.empty()) {
        lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = &image_;
        lv_area_t area{0, 0, width_ - 1, height_ - 1};
        lv_draw_image(layer, &d, &area);
    } else {
        const float size = height_ * 0.17f, advance = text_.advance("fpvOS") * size / canopy::kFontPixels;
        const float x = (width_ - advance) / 2;
        text_.canvas(layer, "fpv", x, height_ * 0.51f, size, false, theme.text, 1);
        text_.canvas(layer, "OS", x + text_.advance("fpv") * size / canopy::kFontPixels, height_ * 0.51f, size, false, theme.accent, 1);
    }
}
}

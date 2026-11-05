#include "idle_scene.hpp"
#include "hud/hud_theme.hpp"
#include <fstream>
#include <cstring>
#include <stdexcept>

namespace kestrel_ui {
void IdleScene::Image::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return; // Minimal asset sets still have a text-only idle screen.
    uint8_t header[16]{};
    auto word = [&](int offset) { return uint32_t(header[offset]) | uint32_t(header[offset + 1]) << 8 |
        uint32_t(header[offset + 2]) << 16 | uint32_t(header[offset + 3]) << 24; };
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "KUIS", 4))
        throw std::runtime_error("Invalid idle artwork: " + path);
    const auto w = word(4), h = word(8), channels = word(12);
    if (!w || !h || w > 4096 || h > 4096 || (channels != 1 && channels != 4) || uint64_t(w) * h * channels > 32 * 1024 * 1024)
        throw std::runtime_error("Invalid idle artwork bounds: " + path);
    pixels.resize(size_t(w) * h * channels);
    if (!file.read(reinterpret_cast<char*>(pixels.data()), pixels.size()) || file.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Invalid idle artwork length: " + path);
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = channels == 1 ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_ARGB8888;
    descriptor.header.w = w; descriptor.header.h = h; descriptor.header.stride = w * channels;
    descriptor.data_size = pixels.size(); descriptor.data = pixels.data();
}
IdleScene::IdleScene(const std::string& assets, int w, int h)
    : width_(w), height_(h), text_(assets + "/canopy-font.kuf", w, h, 512 * 1024) {
    background_.load(assets + "/idle-background.kui");
    head_.load(assets + "/idle-fpv.kui"); tail_.load(assets + "/idle-os.kui");
}
IdleScene::~IdleScene() {
    for (auto* item : {&background_, &head_, &tail_}) {
        lv_image_cache_drop(&item->descriptor); lv_image_header_cache_drop(&item->descriptor);
    }
}
void IdleScene::image(lv_layer_t* layer, const Image& source, float x, float y, float scale, const float* tint) {
    if (source.pixels.empty()) return;
    lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = &source.descriptor;
    d.scale_x = d.scale_y = uint32_t(std::lround(scale * LV_SCALE_NONE)); d.pivot = {0, 0};
    if (tint) { d.recolor = lv_color_make(uint8_t(tint[0] * 255), uint8_t(tint[1] * 255), uint8_t(tint[2] * 255)); d.recolor_opa = LV_OPA_COVER; }
    lv_area_t area{int32_t(std::lround(x)), int32_t(std::lround(y)), 0, 0};
    area.x2 = area.x1 + source.descriptor.header.w - 1; area.y2 = area.y1 + source.descriptor.header.h - 1;
    lv_draw_image(layer, &d, &area);
}
void IdleScene::draw(lv_layer_t* layer, const Model& model, uint64_t now, float open, int side) {
    if (!model.hud_visible() || !model.show_status(now)) return;
    const auto& theme = hud_theme_at(model.theme);
    const float e = open * open * (3 - 2 * open);
    const float cx = width_ * (0.5f + (side ? -1 : 1) * 0.17f * e);
    if (model.backend.vrx.link_valid(now)) {
        // A radio link is not a decoded picture. Keep the real pipeline state
        // visible, never label this state Ready or claim a lock on a timer.
        if (!model.menu_open) {
            const std::string title = "ACQUIRING VIDEO", status = "WAITING FOR VIDEO";
            const float size = 34 * height_ / 1080.0f;
            text_.canvas(layer, title, (width_ - text_.advance(title) * size / canopy::kFontPixels) / 2,
                         height_ * 0.48f, size, false, theme.text, 1);
            text_.canvas(layer, status, (width_ - text_.advance(status) * size * 0.5f / canopy::kFontPixels) / 2,
                         height_ * 0.53f, size * 0.5f, false, theme.accent, 1);
        }
        return;
    }
    if (!background_.pixels.empty()) image(layer, background_, 0, 0, float(width_) / background_.descriptor.header.w, nullptr);
    if (!head_.pixels.empty() && !tail_.pixels.empty()) {
        float h = height_ * 0.3315f, w = h * 8;
        const float fit = std::min(1.0f, 0.8f * 0.66f * width_ / (w * 0.5f));
        h *= 1 + (fit - 1) * e; w = h * 8;
        const float x = cx - w * 0.5f, y = height_ * 0.46f - h * 0.5f, sh = h * 0.018f;
        const float shadow1[] = {0, 0, 0.05f}, shadow2[] = {0, 0, 0.03f};
        for (const auto* mask : {&head_, &tail_}) {
            image(layer, *mask, x + sh * 2, y + sh * 2, h / 256, shadow1);
            image(layer, *mask, x + sh, y + sh, h / 256, shadow2);
        }
        image(layer, head_, x, y, h / 256, theme.text); image(layer, tail_, x, y, h / 256, theme.accent);
    } else {
        const float size = height_ * 0.17f, advance = text_.advance("fpvOS") * size / canopy::kFontPixels;
        const float x = cx - advance / 2;
        text_.canvas(layer, "fpv", x, height_ * 0.51f, size, false, theme.text, 1);
        text_.canvas(layer, "OS", x + text_.advance("fpv") * size / canopy::kFontPixels, height_ * 0.51f, size, false, theme.accent, 1);
    }
    const float size = height_ * 45.8f / 1080;
    const float x = cx - text_.advance("Ready") * size / canopy::kFontPixels / 2, y = height_ * 0.867f;
    const float shadow1[] = {0, 0, 0.05f}, shadow2[] = {0, 0, 0.03f};
    const float sh = height_ * 0.00238f;
    text_.canvas(layer, "Ready", x + sh * 2, y + sh * 2, size, false, shadow1, 0.35f);
    text_.canvas(layer, "Ready", x + sh, y + sh, size, false, shadow2, 0.60f);
    text_.canvas(layer, "Ready", x, y, size, false, theme.accent, 1);
}
}

#pragma once
#include "hud_buffer.hpp"
#include "hud_renderer.hpp"
#include "perspective_text.hpp"
#include "lvgl.h"

namespace kestrel_ui {
// Passive LVGL View: it draws HudData exactly as provided. No Model, backend,
// input socket, telemetry parsing, command path or framebuffer access.
class HudView final : public HudRenderer {
public:
    HudView(int width, int height, const std::string& assets);
    ~HudView() override;
    const hud_ipc::Layout& layout() const override { return layout_; }
    void render(const HudData& data, HudBuffer& buffer, uint32_t slot) override;
    size_t cache_bytes() const override { return text_.cached_bytes(); }
    uint64_t rasters() const override { return text_.rasterizations(); }
private:
    struct Asset {
        std::vector<uint8_t> pixels;
        lv_image_dsc_t image{};
        int x = 0, y = 0;
        int design_w = 0, design_h = 0;   // the screen the asset was made for
        // An asset made for any screen: drawn scaled to the HUD's (Picture
        // Size draws the HUD smaller than the assets' screen).
        void load(const std::string& path, int width, int height);
    } assets_[2];
    int width_, height_;
    hud_ipc::Layout layout_;
    PerspectiveText text_;
    HudData data_;
    lv_obj_t* screen_ = nullptr;
    std::array<lv_obj_t*, 3> objects_{};
    static void draw_event(lv_event_t* event);
    void panel(lv_layer_t* layer, bool left);
    void status(lv_layer_t* layer);
    void track(lv_layer_t* layer, bool left, float start, float end, float y, float height,
               int segments, float level, const float* rgb, float alpha, float block);
};
} // namespace kestrel_ui

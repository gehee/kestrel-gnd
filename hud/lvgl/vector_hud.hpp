#pragma once
#include "hud_renderer.hpp"
#include "perspective_text.hpp"

namespace kestrel_ui {
// Reference-inspired instrumentation. It renders supplied instruments only;
// the static center sight is decorative, not a measured attitude indicator.
class VectorHud final : public HudRenderer {
public:
    VectorHud(int width, int height, const std::string& assets);
    ~VectorHud() override;
    const hud_ipc::Layout& layout() const override { return layout_; }
    void render(const HudData& data, HudBuffer& buffer, uint32_t slot) override;
    size_t cache_bytes() const override { return text_.cached_bytes(); }
    uint64_t rasters() const override { return text_.rasterizations(); }
private:
    int width_, height_;
    float u_;
    hud_ipc::Layout layout_;
    PerspectiveText text_;
    HudData data_;
    lv_obj_t* screen_ = nullptr;
    std::vector<lv_obj_t*> objects_;
    std::vector<std::vector<uint8_t>> signatures_;
    const HudBuffer* last_buffer_ = nullptr;
    int previous_slot_ = -1;
    static void draw_event(lv_event_t* event);
    void draw(lv_layer_t* layer, size_t region);
    void line(lv_layer_t* layer, float x1, float y1, float x2, float y2, float alpha = 0.65f, float width = 1);
    void arc(lv_layer_t* layer, float x, float y, float radius, float start, float end, float alpha, float width = 1);
    void backing(lv_layer_t* layer, float x, float y, float width, float height, float alpha = 0.10f);
    void text(lv_layer_t* layer, const std::string& value, float x, float baseline, float size,
              float alpha = 0.90f, bool right = false, bool accent = false);
    void centered(lv_layer_t* layer, const std::string& value, float x, float baseline, float size,
                  float alpha = 0.90f, bool accent = false);
    const HudMetric& metric(HudMetricId id) const;
    void row(lv_layer_t* layer, HudMetricId id, float x, float y, float width);
    void gauge(lv_layer_t* layer, const HudMetric& value, bool left);
    std::vector<uint8_t> signature(size_t region) const;
};
} // namespace kestrel_ui

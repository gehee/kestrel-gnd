#pragma once
#include "model.hpp"
#include "controller.hpp"
#include "hud_source.hpp"
#include "perspective_text.hpp"
#include "lvgl.h"
#include <string>
#include <vector>
#include <optional>

namespace kestrel_ui {
class View {
public:
    View(const Controller& controller, int width, int height, const std::string& assets, HudSource* hud = nullptr);
    ~View();
    void update(uint64_t now);
    size_t text_cache_bytes() const { return text_.cached_bytes() + hud_->cache_bytes(); }
    uint64_t text_rasterizations() const { return text_.rasterizations() + hud_->rasters(); }
    bool animating() const { return animating_; }
    uint64_t menu_snapshots() const { return menu_snapshot_count_; }
private:
    const Controller& controller_;
    const Model& model_;
    std::unique_ptr<HudSource> owned_hud_;
    HudSource* hud_ = nullptr;
    hud_ipc::Layout hud_layout_;
    uint64_t hud_revision_ = 0;
    std::vector<lv_obj_t*> hud_objects_;
    std::optional<Model> menu_preview_;
    int width_, height_;
    PerspectiveText text_;
    uint64_t now_ = 0;
    Telemetry presented_;
    bool initialized_ = false, old_fresh_ = false, old_valid_ = false;
    bool old_flight_fresh_ = false;
    uint64_t flight_version_ = 0;
    int old_theme_ = -1, old_selected_ = -1, old_tab_ = -1;
    uint64_t menu_version_ = 0;
    uint64_t rejected_inputs_ = 0;
    bool old_details_ = true;
    bool old_hud_visible_ = true;
    int active_menu_side_ = 0;
    bool old_menu_ = false;
    float menu_progress_ = 0;
    bool animating_ = false;
    bool capturing_menu_ = false;
    lv_draw_buf_t* menu_snapshot_ = nullptr;
    std::string menu_snapshot_signature_;
    uint64_t menu_snapshot_count_ = 0;
    uint64_t menu_snapshot_checked_us_ = 0;
    uint64_t last_tick_ = 0;
    lv_obj_t *screen_ = nullptr, *menu_ = nullptr, *picker_ = nullptr;
    void sync_hud_objects();
    void draw_hud(lv_layer_t* layer, size_t region);
    void draw_menu(lv_layer_t* layer);
    lv_area_t menu_reveal(float progress) const;
    void draw_picker(lv_layer_t* layer);
    void prepare_menu_snapshot();
    void drop_menu_snapshot();
    static void draw_event(lv_event_t* event);
};
} // namespace kestrel_ui

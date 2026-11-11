#pragma once
#include "model.hpp"
#include "controller.hpp"
#include "hud_source.hpp"
#include "perspective_text.hpp"
#include "idle_scene.hpp"
#include "betaflight.hpp"
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
    bool betaflight_available() const { return bf_atlas_.available(); }
private:
    const Controller& controller_;
    const Model& model_;
    std::unique_ptr<HudSource> owned_hud_;
    HudSource* hud_ = nullptr;
    hud_ipc::Layout hud_layout_;
    uint64_t hud_revision_ = 0;
    std::vector<lv_obj_t*> hud_objects_;
    std::optional<Model> menu_preview_;
    std::optional<Model> menu_cached_model_;
    int width_, height_;
    PerspectiveText text_;
    IdleScene idle_;
    BetaflightAtlas bf_atlas_;
    lv_obj_t* bf_object_=nullptr;
    lv_obj_t* debug_object_=nullptr;
    lv_obj_t* fps_object_=nullptr;
    std::string fps_text_;
    std::array<uint16_t,1060> bf_grid_{};
    bool bf_visible_=false;
    std::string debug_text_;
    std::array<int,2> motion_offset_{};
    lv_obj_t* gallery_=nullptr;
    std::string gallery_signature_;
    std::array<lv_image_dsc_t,6> thumbnail_images_{};
    std::array<std::shared_ptr<const media::Thumbnail>,6> thumbnails_{};
    bool old_status_ = false;
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
    // The dimmer's share of the blade: it follows the blade at rest only. A
    // dimmer level that changes with every step of the blade repaints every
    // pixel of the screen, which these receivers cannot do 60 times a second.
    float dimmer_progress_ = 0;
    // Where the picker labels were painted, per side (x1 > x2: not yet known).
    std::array<lv_area_t,2> picker_text_{{{1,1,0,0},{1,1,0,0}}};
    // While the blade moves, nothing lies between the dimmer and the blade's
    // ground in most of it. There the dimmer applies both in one pass: it
    // stores ground-over-dimmer where nothing is below (no read at all), or
    // blends the two over the artwork, HUD or OSD below.
    std::array<lv_area_t,2> solid_{};
    int solids_ = 0;
    bool solid_clear_ = false;
    uint32_t solid_dimmer_ = 0, solid_ground_ = 0, solid_argb_ = 0;
    bool solid_stored_ = false;
    void plan_solid_ground();
    void draw_dimmer(lv_layer_t* layer);
    // The idle artwork under a drawn dimmer: the dimmer stores it already
    // blended (reading the artwork, never the screen), so the artwork is
    // drawn on its own only below HUD regions, which lie between the two.
    bool art_deferred_ = false;
    lv_layer_t* art_layer_ = nullptr;
    lv_area_t art_clip_{};
    std::array<lv_area_t,12> blocked_{};
    int blocked_count_ = 0;
    bool hud_drawn_at(size_t region) const;
    void draw_idle(lv_layer_t* layer);
    void draw_deferred_art(lv_layer_t* layer, const lv_area_t& clip);
    bool animating_ = false;
    bool capturing_menu_ = false;
    bool menu_damage_applied_ = false;
    lv_draw_buf_t* menu_snapshot_ = nullptr;
    std::string menu_snapshot_signature_;
    uint64_t menu_snapshot_count_ = 0;
    uint64_t menu_snapshot_checked_us_ = 0;
    uint64_t last_tick_ = 0;
    lv_obj_t* channel_ = nullptr;
    lv_obj_t* clock_ = nullptr;
    std::string clock_text_, recording_text_;
    std::array<lv_obj_t*,2> edge_objects_{};
    std::array<std::vector<uint8_t>,2> edge_pixels_;
    std::array<lv_image_dsc_t,2> edge_images_{};
    float edge_fade_ = 0;
    int edge_level_ = 0, edge_opacity_ = 0;
    void draw_edge(lv_layer_t*, int side);
    uint64_t channel_revision_ = 0;
    bool old_channel_ = false;
    lv_obj_t *screen_ = nullptr, *menu_ = nullptr, *picker_ = nullptr, *idle_object_ = nullptr, *dimmer_ = nullptr;
    lv_obj_t *capture_screen_=nullptr,*menu_capture_=nullptr;
    struct MenuLayout {
        float u,help_y;
        int pad,row_h,list_top,extra,start,visible;
        std::vector<MenuRow> rows;
        std::vector<std::string> help_lines;
    };
    std::optional<MenuLayout> menu_cached_layout_;
    const MenuLayout* capture_layout_=nullptr;
    // The layout of the frame being drawn. LVGL draws the blade once per
    // damaged area between two updates; lay it out once.
    struct FrameLayout { const Model* model; int width, height; MenuLayout layout; };
    std::optional<FrameLayout> frame_layout_;
    MenuLayout menu_layout(const Model&,int width,int height) const;
    void sync_hud_objects();
    void draw_hud(lv_layer_t* layer, size_t region);
    void draw_menu(lv_layer_t* layer);
    lv_area_t menu_reveal(float progress) const;
    void draw_picker(lv_layer_t* layer);
    void draw_channel(lv_layer_t* layer);
    void draw_gallery(lv_layer_t* layer);
    void prepare_menu_snapshot();
    void drop_menu_snapshot();
    static void draw_event(lv_event_t* event);
};
} // namespace kestrel_ui

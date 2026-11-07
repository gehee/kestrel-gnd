#pragma once
#include "render/software/canopy.hpp"
#include "backend_state.hpp"
#include "hud_data.hpp"
#include "clock.hpp"
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <ctime>

namespace kestrel_ui {

enum class RcSource { Unavailable, LinkQuality, Rssi };
struct RcSignal {
    RcSource source = RcSource::Unavailable;
    int percent = 0;
    const char* label() const {
        return source == RcSource::LinkQuality ? "RC LQ" : source == RcSource::Rssi ? "RC RSSI" : "RC --";
    }
    float level() const { return percent / 100.0f; }
};

enum class Key { Menu, Up, Down, Left, Right, Enter, Back, Quit, Record };
enum class MenuStage { Pick, Browse, Edit };
enum class SceneMode { Live, Menu, GalleryBrowse, GalleryPlayback };
struct UiPreferences {
    int theme = 0, fps = 25;
    bool details = true, repeat = true, hud = true, per_cell = true;
    int clock = 0;
    bool bf_osd = false, debug = false;
    int dynamic = 0;
    int ui_scale = 10;
    bool link_edge_warning = true;
};
struct MenuRow {
    std::string name, value;
    bool editable = false;
    std::string id, help;
    std::vector<std::string> options;
    bool disabled = false, heading = false, action = false;
};

// Graphics-independent menu state. Rows follow the VRX Pro UI; unsupported
// controls remain disabled until the platform provides a working backend.
class Model {
public:
    BackendState backend;
    SceneMode scene = SceneMode::Live;
    bool menu_open = false, quit = false;
    bool channel_open = false;
    int channel_mhz = 0;
    int gallery_selected=0;
    std::string gallery_delete_name;
    bool gallery_delete_chosen=false;
    int tab = 0, selected = 0, theme = 0;
    int side = 0, fps = 25;
    int clock = 0;
    bool bf_osd = false, debug = false;
    int dynamic = 0;
    int ui_scale = 10;
    bool link_edge_warning = true;
    bool playback_hud_enabled = true;
    bool playback_controls_visible = true;
    bool show_details = true, key_repeat = true, show_hud = true, per_cell = true;
    MenuStage stage = MenuStage::Pick;
    uint64_t menu_version = 0, key_count = 0, rejected_inputs = 0;
    std::string last_key = "--", input_sources = "stdin", control_path;
    std::string feedback;
    std::string build = "--", version = "--", decoder = "--", screen_mode = "--";
    int display_width = 1920, display_height = 1080;
    int hud_pid = -1;
    uint64_t hud_restarts = 0;
    bool simulated = false;
    std::string hud_style = "canopy";
    canopy::LinkVisual visual;
    UiPreferences preferences() const { return {theme, fps, show_details, key_repeat, show_hud, per_cell, clock, bf_osd, debug, dynamic, ui_scale, link_edge_warning}; }
    std::string clock_text(std::time_t wall_time, uint64_t now) const;
    void set_preferences(const UiPreferences& p);
    std::vector<int> side_tabs() const;
    int first_row(uint64_t now) const;
    int remembered_section() const;
    std::string describe() const;
    std::string selected_help(uint64_t now) const;
    int option_index() const;
    bool pending(const std::string& id) const { return pending_.count(id) != 0; }
    std::string status(uint64_t now) const;
    bool show_status(uint64_t now) const;
    RcSignal rc_signal(uint64_t now) const;
    SceneMode scene_mode() const;
    bool hud_visible() const;
    std::vector<MenuRow> rows(uint64_t now) const;
    static const char* tab_name(int tab);
private:
    friend class Controller;
    int remembered_tab_[2] = {2, 0}, edit_original_ = 0;
    int edit_tab_ = -1;
    std::string edit_id_;
    std::map<std::string, int> pending_;
    std::map<std::string, int> receiver_preview_;
    int setting(const std::string& id) const;
    void set_setting(const std::string& id, int value);
    bool edit_intact() const;
};
const char* key_name(Key key);
bool parse_key(const std::string& text, Key& key);
} // namespace kestrel_ui

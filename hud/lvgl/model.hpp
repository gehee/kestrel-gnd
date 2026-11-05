#pragma once
#include "render/software/canopy.hpp"
#include "backend_state.hpp"
#include "hud_data.hpp"
#include "clock.hpp"
#include <string>
#include <vector>
#include <functional>

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

enum class Key { Menu, Up, Down, Left, Right, Enter, Back, Quit };
enum class MenuStage { Pick, Browse, Edit };
enum class SceneMode { Live, Menu, GalleryBrowse, GalleryPlayback };
struct UiPreferences {
    int theme = 0, fps = 25;
    bool details = true, repeat = true;
};
struct MenuRow {
    std::string name, value;
    bool editable = false;
    std::string id, help;
    std::vector<std::string> options;
};

// A graphics-independent initial OSD facade. Backend settings remain backend
// owned. Only local visual preferences are editable until stock setters and
// their readback/persistence semantics have been validated.
class Model {
public:
    BackendState backend;
    SceneMode scene = SceneMode::Live;
    bool menu_open = false, quit = false;
    int tab = 0, selected = 0, theme = 0;
    int side = 0, fps = 25;
    bool show_details = true, key_repeat = true;
    MenuStage stage = MenuStage::Pick;
    uint64_t menu_version = 0, key_count = 0, rejected_inputs = 0;
    std::string last_key = "--", input_sources = "stdin", control_path;
    std::string feedback;
    int display_width = 1920, display_height = 1080;
    int hud_pid = -1;
    uint64_t hud_restarts = 0;
    bool simulated = false;
    std::string hud_style = "canopy";
    canopy::LinkVisual visual;
    UiPreferences preferences() const { return {theme, fps, show_details, key_repeat}; }
    void set_preferences(const UiPreferences& p);
    std::vector<int> side_tabs() const;
    int remembered_section() const;
    std::string describe() const;
    std::string selected_help(uint64_t now) const;
    int option_index() const;
    std::string status(uint64_t now) const;
    RcSignal rc_signal(uint64_t now) const;
    SceneMode scene_mode() const;
    bool hud_visible() const;
    std::vector<MenuRow> rows(uint64_t now) const;
    static const char* tab_name(int tab);
private:
    friend class Controller;
    int remembered_tab_[2] = {1, 0}, edit_original_ = 0;
    int edit_tab_ = -1;
    std::string edit_id_;
    int setting(const std::string& id) const;
    void set_setting(const std::string& id, int value);
    bool edit_intact() const;
};
const char* key_name(Key key);
bool parse_key(const std::string& text, Key& key);
} // namespace kestrel_ui

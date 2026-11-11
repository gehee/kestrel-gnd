#pragma once
#include "model.hpp"
#include "link_warning.hpp"

namespace kestrel_ui {
class Controller {
public:
    explicit Controller(Model& model) : model_(model) {}
    Model& model() { return model_; }
    const Model& model() const { return model_; }
    void tick(uint64_t now);
    void key(Key key, bool repeat = false);
    void open_menu();
    void set_scene(SceneMode scene);
    // The craft's arm state: 1 armed, 0 disarmed, -1 not known (no word from
    // the FC within 2 s).
    int arm_state(uint64_t now) const;
    bool armed(uint64_t now) const { return arm_state(now) == 1; }
    // True once after each arming tick() saw - a change from disarmed: the
    // first word after a start, or after the FC went quiet, is none.
    bool take_arming() { const bool a = armed_edge_; armed_edge_ = false; return a; }
    HudData hud_data(uint64_t now,bool recording_snapshot=false) const;
    int link_warning() const { return warning_.level(); }
    std::string recording_text(uint64_t now) const;
    std::function<void(const UiPreferences&)> save_preferences;
    std::function<std::string(const std::string&, int)> apply_receiver;
private:
    Model& model_;
    bool previous_ = false;
    uint64_t up_ = 0, down_ = 0;
    float cell_peak_ = 0;
    LinkWarning warning_;
    uint64_t recording_since_ = 0, armed_since_ = 0;
    std::string recording_id_;
    bool was_recording_ = false, was_armed_ = false;
    int arm_seen_ = -1;          // the last arm state known (not -1)
    bool armed_edge_ = false;    // an arming not taken yet (take_arming)
    void cancel_edit();
};
} // namespace kestrel_ui

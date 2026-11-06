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
    void cancel_edit();
};
} // namespace kestrel_ui

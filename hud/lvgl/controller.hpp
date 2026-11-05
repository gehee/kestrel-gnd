#pragma once
#include "model.hpp"

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
    HudData hud_data(uint64_t now) const;
    std::function<void(const UiPreferences&)> save_preferences;
private:
    Model& model_;
    bool previous_ = false;
    uint64_t up_ = 0, down_ = 0;
    float cell_peak_ = 0;
    void cancel_edit();
};
} // namespace kestrel_ui

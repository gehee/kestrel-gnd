#pragma once
#include "perspective_text.hpp"
#include "model.hpp"

namespace kestrel_ui {
// One build-time raster per theme. No logo/background rasterization on target.
class IdleScene {
public:
    IdleScene(const std::string& assets, int width, int height);
    ~IdleScene();
    void prepare(const Model& model, uint64_t now);
    void draw(lv_layer_t* layer, const Model& model, uint64_t now);
private:
    std::string path_;
    int width_, height_;
    std::string loaded_theme_;
    std::vector<uint8_t> pixels_;
    lv_image_dsc_t image_{};
    PerspectiveText text_;
    void drop_image();
};
}

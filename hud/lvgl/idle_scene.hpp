#pragma once
#include "perspective_text.hpp"
#include "model.hpp"
#include <memory>
#include <vector>

namespace kestrel_ui {
// One build-time raster per theme. No logo/background rasterization on target.
class IdleScene {
public:
    // width x height: the UI's own area. The artwork is the whole screen's
    // (screen_w x screen_h, 0: the same), the UI's area at x, y in it
    // (Picture Size): it stays full screen, the UI showing its part of it.
    IdleScene(const std::string& assets, int width, int height, int screen_w = 0, int screen_h = 0, int x = 0, int y = 0);
    ~IdleScene();
    void prepare(const Model& model, uint64_t now);
    void draw(lv_layer_t* layer, const Model& model, uint64_t now);
    // Whether draw() puts any pixel on the screen for this model.
    bool paints(const Model& model, uint64_t now) const;
    // The artwork's pixels where the UI's area starts (ARGB8888, art_stride()
    // pixels a row) when draw() puts exactly that image on the whole area,
    // else null.
    const uint32_t* artwork(const Model& model, uint64_t now) const;
    int art_stride() const { return screen_w_; }
    // The whole screen's artwork (screen_w x screen_h) while artwork() is not
    // null, held for as long as the caller keeps it (a theme change drops
    // ours): what shows around a smaller UI.
    std::shared_ptr<const std::vector<uint8_t>> screen_artwork(const Model& model, uint64_t now) const;
private:
    std::string path_;
    int width_, height_, screen_w_, screen_h_, x_, y_;
    std::string loaded_theme_;
    std::shared_ptr<std::vector<uint8_t>> pixels_;
    lv_image_dsc_t image_{};
    PerspectiveText text_;
    void drop_image();
};
}

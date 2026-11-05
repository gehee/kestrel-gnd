#pragma once
#include "perspective_text.hpp"
#include "model.hpp"

namespace kestrel_ui {
// Prebaked original artwork; no PNG/Cairo dependency on the receiver.
class IdleScene {
public:
    IdleScene(const std::string& assets, int width, int height);
    ~IdleScene();
    void begin_frame() { text_.begin_frame(); }
    void draw(lv_layer_t* layer, const Model& model, uint64_t now, float open, int side);
private:
    struct Image {
        lv_image_dsc_t descriptor{};
        std::vector<uint8_t> pixels;
        void load(const std::string& path);
    } background_, head_, tail_;
    int width_, height_;
    PerspectiveText text_;
    void image(lv_layer_t*, const Image&, float x, float y, float scale, const float* tint);
};
}

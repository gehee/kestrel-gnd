// Unit test of render::Canvas: its default arguments are the ones the OSD's
// own GL functions had (osd.hpp), since every screen now draws through the
// canvas and leans on them - a default that drifted would recolour the HUD
// without a compile error. Checked with a canvas that records its calls.
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <cstdio>
#include <string>

#include "render/canvas.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Recorder : render::Canvas {
    std::string last;
    float f[8] = {};
    bool flag = false;
    int n[2] = {};
    void panel(float, float, float, float, float a, float r, float g, float b) override {
        last = "panel"; f[0] = a; f[1] = r; f[2] = g; f[3] = b;
    }
    void trapezoid(float, float, float, float, float, float, float, float, float, float) override {}
    void poly(const std::vector<math::Vec2> &, float, float, float, float) override {}
    void hex_panel(float, float, float, float, float, float, float, float, bool, int nh, int nv,
                   float gy, float gh, bool ux) override {
        last = "hex_panel"; n[0] = nh; n[1] = nv; f[0] = gy; f[1] = gh; flag = ux;
    }
    void arc(float, float, float, float, float, float, float, float, float, float) override {}
    void grid_arc(float, float, float, float, float, float, float) override {}
    void glow_line(const std::vector<math::Vec2> &, float, float, float, float, float) override {}
    void icon(render::ImageId, float, float, float, float, float r, float g, float b, float a) override {
        last = "icon"; f[0] = r; f[1] = g; f[2] = b; f[3] = a;
    }
    void icon_quad(render::ImageId, math::Vec2, math::Vec2, math::Vec2, math::Vec2, float r, float g, float b) override {
        last = "icon_quad"; f[0] = r; f[1] = g; f[2] = b;
    }
    float text_width(const char *, float) override { return 0; }
    void text(const char *, float, float, float, bool right, float r, float g, float b) override {
        last = "text"; flag = right; f[0] = r; f[1] = g; f[2] = b;
    }
    void text_wipe(const char *, float, float, float, bool, float, float, float, float) override {}
    void stacked_bar(float, float, float, float, float, bool, float, float, float, float,
                     float v2, float, float, float, float, float, float, float, float, float, float, float,
                     float, float, float, float, float v6, float, float, float) override {
        last = "stacked_bar"; f[0] = v2; f[1] = v6;
    }
    void signal_bar(float, float, float, float, float, float, float) override {}
    void seg_track(float, float, float, float, int, float, bool, float, float, float, float, float,
                   float reveal) override {
        last = "seg_track"; f[0] = reveal;
    }
    void fade_wedge(const float *, const float *, const float *, const float *, int, float, float, float) override {}
    void fade_rule(float, float, float, float, float, float, float, float, float, float a1) override {
        last = "fade_rule"; f[0] = a1;
    }
    void rail_glow(float, float, float, float, float, float, float) override {}
    void set_feather(float) override {}
    // The rest draws nothing here.
    void bind_thread() override {}
    void init() override {}
    void cache_text(const char*) override {}
    void begin_frame(int, int) override {}
    void viewport(int, int) override {}
    void clip(int, int, int, int) override {}
    void unclip() override {}
    void clear() override {}
    void blend_over() override {}
    void set_transform(const float*) override {}
    void set_fade(float) override {}
    void image_quad(render::ImageId, const float*, float, float, float, float) override {}
    render::ImageId make_image(int, int, const void*, bool repeat) override { flag = repeat; return 1; }
    void update_image(render::ImageId, int, int, const void*) override {}
    void free_image(render::ImageId) override {}
    bool video_supported() const override { return false; }
    render::ImageId make_video() override { return 0; }
    bool import_video(render::ImageId, const render::VideoFrame&, unsigned*) override { return false; }
    void video_quad(render::ImageId, const float*, const float*, float) override {}
    void image_warp_quad(render::ImageId, const float*, const float*, float) override {}
    bool snapshot(render::ImageId&, int, int) override { return false; }
    bool begin_layer(render::ImageId&, int, int) override { return false; }
    void end_layer(int, int) override {}
    void copy_quad(render::ImageId, const float*, const float*, float) override {}
    void triangles(const float*, int, const float*, bool strip) override { flag = strip; }
    void solid_quad(const float*, float, float, float, float) override {}
    float text_quad(const char*, float, float, float, int, const float*, float) override { return 0; }
};

int main() {
    Recorder rec;
    render::Canvas &c = rec;

    c.panel(0, 0, 1, 1, 0.5f);
    CHECK(rec.last == "panel" && rec.f[0] == 0.5f && rec.f[1] == 0.1f && rec.f[2] == 0.1f && rec.f[3] == 0.15f);

    c.hex_panel(0, 0, 1, 1, 1, 0, 0, 0, false);
    CHECK(rec.n[0] == 8 && rec.n[1] == 10 && rec.f[0] == -1.0f && rec.f[1] == -1.0f && !rec.flag);

    c.icon(7, 0, 0, 1, 1);
    CHECK(rec.last == "icon" && rec.f[0] == 1 && rec.f[1] == 1 && rec.f[2] == 1 && rec.f[3] == 1);

    c.icon_quad(7, {0, 0}, {1, 0}, {1, 1}, {0, 1});
    CHECK(rec.last == "icon_quad" && rec.f[0] == 1 && rec.f[1] == 1 && rec.f[2] == 1);

    c.text("x", 0, 0, 1);
    CHECK(rec.last == "text" && !rec.flag && rec.f[0] == 1 && rec.f[1] == 1 && rec.f[2] == 1);

    // An image clamps at its edges, and triangles are separate ones, unless asked.
    rec.flag = true;
    c.make_image(1, 1, nullptr);
    CHECK(!rec.flag);
    rec.flag = true;
    c.triangles(nullptr, 0, nullptr);
    CHECK(!rec.flag);

    c.stacked_bar(0, 0, 1, 1, 0, false, 0.5f, 1, 1, 1);
    CHECK(rec.last == "stacked_bar" && rec.f[0] == 0 && rec.f[1] == 0);

    c.seg_track(0, 0, 1, 1, 10, 0.5f, false, 1, 1, 1, 0.2f, 1.0f);
    CHECK(rec.last == "seg_track" && rec.f[0] == 1.0f);

    c.fade_rule(0, 0, 1, 0, 0.01f, 1, 1, 1, 1);
    CHECK(rec.last == "fade_rule" && rec.f[0] == 0.0f);

    std::printf(fails ? "canvas_test: %d FAILED\n" : "canvas_test: all passed\n", fails);
    return fails ? 1 : 0;
}

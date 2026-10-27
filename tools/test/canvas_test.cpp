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

    c.stacked_bar(0, 0, 1, 1, 0, false, 0.5f, 1, 1, 1);
    CHECK(rec.last == "stacked_bar" && rec.f[0] == 0 && rec.f[1] == 0);

    c.seg_track(0, 0, 1, 1, 10, 0.5f, false, 1, 1, 1, 0.2f, 1.0f);
    CHECK(rec.last == "seg_track" && rec.f[0] == 1.0f);

    c.fade_rule(0, 0, 1, 0, 0.01f, 1, 1, 1, 1);
    CHECK(rec.last == "fade_rule" && rec.f[0] == 0.0f);

    std::printf(fails ? "canvas_test: %d FAILED\n" : "canvas_test: all passed\n", fails);
    return fails ? 1 : 0;
}

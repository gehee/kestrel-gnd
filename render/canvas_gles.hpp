#pragma once
// The Canvas on the GPU: the OSD's own GLES drawing, unchanged. For now it
// calls the OSD's GL functions, which hold the shaders, buffers and glyph
// cache they share with the rest of the OSD's GL; the screens no longer call
// those directly, so they can move in here without touching a screen.

#include "render/canvas.hpp"

class OSD;

namespace render {

class CanvasGles : public Canvas {
public:
    explicit CanvasGles(OSD *osd) : osd_(osd) {}

    void panel(float x, float y, float w, float h, float alpha, float r, float g, float b) override;
    void trapezoid(float x, float y, float w, float h, float top_scale, float slant,
                   float alpha, float r, float g, float b) override;
    void poly(const std::vector<math::Vec2> &points, float alpha, float r, float g, float b) override;
    void hex_panel(float x, float y, float w, float h, float alpha, float r, float g, float b,
                   bool outline, int num_h, int num_v, float grid_y, float grid_h, bool uniform_x) override;
    void arc(float cx, float cy, float r_in, float r_out, float start_angle, float end_angle,
             float alpha, float r, float g, float b) override;
    void grid_arc(float cx, float cy, float r_inner, float r_outer, float start_angle,
                  float end_angle, float alpha) override;
    void glow_line(const std::vector<math::Vec2> &points, float width, float alpha,
                   float r, float g, float b) override;
    void icon(ImageId image, float x, float y, float w, float h, float r, float g, float b, float alpha) override;
    void icon_quad(ImageId image, math::Vec2 bl, math::Vec2 br, math::Vec2 tr, math::Vec2 tl,
                   float r, float g, float b) override;
    float text_width(const char *text, float scale) override;
    void text(const char *text, float x, float y, float scale, bool right_align, float r, float g, float b) override;
    void text_wipe(const char *text, float x, float y, float scale, bool right_align,
                   float r, float g, float b, float reveal) override;
    void stacked_bar(float x, float y, float w, float h, float slant, bool reverse,
                     float val1, float r1, float g1, float b1, float val2, float r2, float g2, float b2,
                     float val3, float r3, float g3, float b3, float val4, float r4, float g4, float b4,
                     float val5, float r5, float g5, float b5, float val6, float r6, float g6, float b6) override;
    void signal_bar(float x, float y, float w, float h, float value, float min_val, float max_val) override;
    void seg_track(float x, float y, float w, float h, int segs, float filled, bool edge_left,
                   float r, float g, float b, float a_lo, float a_hi, float reveal) override;
    void fade_wedge(const float *xs, const float *tops, const float *bots,
                    const float *alphas, int n, float r, float g, float b) override;
    void fade_rule(float x0, float y0, float x1, float y1, float thick,
                   float a0, float r, float g, float b, float a1) override;
    void rail_glow(float fw, float fh, float depth, float alpha, float r, float g, float b) override;

private:
    OSD *osd_;
};

}  // namespace render

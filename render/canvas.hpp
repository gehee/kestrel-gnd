#pragma once
// The 2D drawing the HUD, menu and gallery are made of: panels, text, arcs,
// bars, icons. The screens draw through this and nothing else for these
// shapes, so a box can supply its own way of drawing them: CanvasGles on the
// GPU (render/canvas_gles), and later a CPU canvas for boxes without one.
//
// Coordinates and colours are as the OSD has always used them: its frustum
// space, colours 0..1, alpha 0..1. An image is the backend's handle for a
// picture it already holds (a GL texture name for CanvasGles).

#include <cstdint>
#include <vector>

#include "utils/math_utils.hpp"

namespace render {

using ImageId = uint32_t;

class Canvas {
public:
    virtual ~Canvas() = default;

    virtual void panel(float x, float y, float w, float h, float alpha,
                       float r = 0.1f, float g = 0.1f, float b = 0.15f) = 0;
    virtual void trapezoid(float x, float y, float w, float h, float top_scale, float slant,
                           float alpha, float r, float g, float b) = 0;
    virtual void poly(const std::vector<math::Vec2> &points, float alpha, float r, float g, float b) = 0;
    virtual void hex_panel(float x, float y, float w, float h, float alpha, float r, float g, float b,
                           bool outline, int num_h = 8, int num_v = 10, float grid_y = -1.0f,
                           float grid_h = -1.0f, bool uniform_x = false) = 0;
    virtual void arc(float cx, float cy, float r_in, float r_out, float start_angle, float end_angle,
                     float alpha, float r, float g, float b) = 0;
    virtual void grid_arc(float cx, float cy, float r_inner, float r_outer, float start_angle,
                          float end_angle, float alpha) = 0;
    virtual void glow_line(const std::vector<math::Vec2> &points, float width, float alpha,
                           float r, float g, float b) = 0;

    virtual void icon(ImageId image, float x, float y, float w, float h,
                      float r = 1.0f, float g = 1.0f, float b = 1.0f, float alpha = 1.0f) = 0;
    virtual void icon_quad(ImageId image, math::Vec2 bl, math::Vec2 br, math::Vec2 tr, math::Vec2 tl,
                           float r = 1.0f, float g = 1.0f, float b = 1.0f) = 0;

    virtual float text_width(const char *text, float scale) = 0;
    virtual void text(const char *text, float x, float y, float scale, bool right_align = false,
                      float r = 1.0f, float g = 1.0f, float b = 1.0f) = 0;
    // The first `reveal` (0..1) of the text, for text being typed out.
    virtual void text_wipe(const char *text, float x, float y, float scale, bool right_align,
                           float r, float g, float b, float reveal) = 0;

    // Up to six coloured segments end to end in one slanted bar.
    virtual void stacked_bar(float x, float y, float w, float h, float slant, bool reverse,
                             float val1, float r1, float g1, float b1,
                             float val2 = 0, float r2 = 0, float g2 = 0, float b2 = 0,
                             float val3 = 0, float r3 = 0, float g3 = 0, float b3 = 0,
                             float val4 = 0, float r4 = 0, float g4 = 0, float b4 = 0,
                             float val5 = 0, float r5 = 0, float g5 = 0, float b5 = 0,
                             float val6 = 0, float r6 = 0, float g6 = 0, float b6 = 0) = 0;
    virtual void signal_bar(float x, float y, float w, float h, float value, float min_val, float max_val) = 0;
    virtual void seg_track(float x, float y, float w, float h, int segs, float filled, bool edge_left,
                           float r, float g, float b, float a_lo, float a_hi, float reveal = 1.0f) = 0;

    // A fading shape: columns xs[i] with their top and bottom and alpha.
    virtual void fade_wedge(const float *xs, const float *tops, const float *bots,
                            const float *alphas, int n, float r, float g, float b) = 0;
    // A hairline ramping from a0 to a1.
    virtual void fade_rule(float x0, float y0, float x1, float y1, float thick,
                           float a0, float r, float g, float b, float a1 = 0.0f) = 0;
    // Glowing rails along the picture's top and bottom edges.
    virtual void rail_glow(float fw, float fh, float depth, float alpha, float r, float g, float b) = 0;
};

}  // namespace render

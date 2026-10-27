#include "render/canvas_gles.hpp"

#include "hud/osd.hpp"

namespace render {

void CanvasGles::panel(float x, float y, float w, float h, float alpha, float r, float g, float b) {
    osd_->draw_panel(x, y, w, h, alpha, r, g, b);
}
void CanvasGles::trapezoid(float x, float y, float w, float h, float top_scale, float slant,
                           float alpha, float r, float g, float b) {
    osd_->draw_trapezoid(x, y, w, h, top_scale, slant, alpha, r, g, b);
}
void CanvasGles::poly(const std::vector<math::Vec2> &points, float alpha, float r, float g, float b) {
    osd_->draw_poly(points, alpha, r, g, b);
}
void CanvasGles::hex_panel(float x, float y, float w, float h, float alpha, float r, float g, float b,
                           bool outline, int num_h, int num_v, float grid_y, float grid_h, bool uniform_x) {
    osd_->draw_hex_panel(x, y, w, h, alpha, r, g, b, outline, num_h, num_v, grid_y, grid_h, uniform_x);
}
void CanvasGles::arc(float cx, float cy, float r_in, float r_out, float start_angle, float end_angle,
                     float alpha, float r, float g, float b) {
    osd_->draw_arc(cx, cy, r_in, r_out, start_angle, end_angle, alpha, r, g, b);
}
void CanvasGles::grid_arc(float cx, float cy, float r_inner, float r_outer, float start_angle,
                          float end_angle, float alpha) {
    osd_->draw_grid_arc(cx, cy, r_inner, r_outer, start_angle, end_angle, alpha);
}
void CanvasGles::glow_line(const std::vector<math::Vec2> &points, float width, float alpha,
                           float r, float g, float b) {
    osd_->draw_glow_line(points, width, alpha, r, g, b);
}
void CanvasGles::icon(ImageId image, float x, float y, float w, float h, float r, float g, float b, float alpha) {
    osd_->draw_icon((GLuint)image, x, y, w, h, r, g, b, alpha);
}
void CanvasGles::icon_quad(ImageId image, math::Vec2 bl, math::Vec2 br, math::Vec2 tr, math::Vec2 tl,
                           float r, float g, float b) {
    osd_->draw_icon_quad((GLuint)image, bl, br, tr, tl, r, g, b);
}
float CanvasGles::text_width(const char *text, float scale) {
    return osd_->text_width(text, scale);
}
void CanvasGles::text(const char *text, float x, float y, float scale, bool right_align, float r, float g, float b) {
    osd_->draw_text(text, x, y, scale, right_align, r, g, b);
}
void CanvasGles::text_wipe(const char *text, float x, float y, float scale, bool right_align,
                           float r, float g, float b, float reveal) {
    osd_->draw_text_wipe(text, x, y, scale, right_align, r, g, b, reveal);
}
void CanvasGles::stacked_bar(float x, float y, float w, float h, float slant, bool reverse,
                             float val1, float r1, float g1, float b1, float val2, float r2, float g2, float b2,
                             float val3, float r3, float g3, float b3, float val4, float r4, float g4, float b4,
                             float val5, float r5, float g5, float b5, float val6, float r6, float g6, float b6) {
    osd_->draw_stacked_bar(x, y, w, h, slant, reverse, val1, r1, g1, b1, val2, r2, g2, b2,
                           val3, r3, g3, b3, val4, r4, g4, b4, val5, r5, g5, b5, val6, r6, g6, b6);
}
void CanvasGles::signal_bar(float x, float y, float w, float h, float value, float min_val, float max_val) {
    osd_->draw_signal_bar(x, y, w, h, value, min_val, max_val);
}
void CanvasGles::seg_track(float x, float y, float w, float h, int segs, float filled, bool edge_left,
                           float r, float g, float b, float a_lo, float a_hi, float reveal) {
    osd_->draw_seg_track(x, y, w, h, segs, filled, edge_left, r, g, b, a_lo, a_hi, reveal);
}
void CanvasGles::fade_wedge(const float *xs, const float *tops, const float *bots,
                            const float *alphas, int n, float r, float g, float b) {
    osd_->draw_fade_wedge(xs, tops, bots, alphas, n, r, g, b);
}
void CanvasGles::fade_rule(float x0, float y0, float x1, float y1, float thick,
                           float a0, float r, float g, float b, float a1) {
    osd_->draw_fade_rule(x0, y0, x1, y1, thick, a0, r, g, b, a1);
}
void CanvasGles::rail_glow(float fw, float fh, float depth, float alpha, float r, float g, float b) {
    osd_->draw_rail_glow(fw, fh, depth, alpha, r, g, b);
}

}  // namespace render

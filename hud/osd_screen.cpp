// Pixel-space drawing for the pages that open over the live picture: the
// settings panel and the channel page.
//
// The same pieces the gallery draws with (osd_gallery.cpp), with the same
// meaning: positions in screen pixels from the top left with y down, a fill
// that lands as plain premultiplied colour, text placed by its baseline, a
// frame, a pill. px_begin() sets the pixel projection - optionally scaled
// about a point, which is how a page grows in as it opens - and a fade that
// every piece is multiplied by.
//
// Flat shapes (fills, frames, hatching) are not drawn one by one: they are
// gathered into one list of coloured triangles and drawn in a single call
// when something else has to be drawn on top - text, the live picture - or
// when the page is done (px_flush). A page of a few hundred bars and ticks
// costs a handful of draws instead of one each.
//
// The gallery still has its own copies of these as lambdas; they draw the
// same way.

#include "hud/osd.hpp"

#include <algorithm>
#include <cmath>

void OSD::px_begin(int W, int H, float cx, float cy, float sc, float alpha) {
    px_flush();                         // what is gathered was laid out for the old projection
    px_W_ = W; px_H_ = H;
    px_cx_ = cx; px_cy_ = cy; px_sc_ = sc; px_a_ = alpha;
    // x' = cx + (x - cx) * sc, in GL's y-up pixels, then to clip space.
    const float gcy = (float)H - cy;
    const float m[16] = { 2.0f * sc / W, 0, 0, 0,
                          0, 2.0f * sc / H, 0, 0,
                          0, 0, 1, 0,
                          2.0f * cx * (1.0f - sc) / W - 1.0f, 2.0f * gcy * (1.0f - sc) / H - 1.0f, 0, 1 };
    std::copy(m, m + 16, px_mvp_);
    canvas_->blend_over();
    canvas_->set_transform(px_mvp_);
    canvas_->set_fade(1.0f);
}

// The gathered shapes, in one draw: x, y and the premultiplied colour per vertex.
void OSD::px_flush() {
    if (px_batch_.empty()) return;
    canvas_->triangles(px_batch_.data(), (int)(px_batch_.size() / 6), px_mvp_);
    px_batch_.clear();
}

// One vertex: y turned up, colour premultiplied.
void OSD::px_vertex(float x, float y, const float* c, float a) {
    px_batch_.insert(px_batch_.end(), { x, (float)px_H_ - y, c[0] * a, c[1] * a, c[2] * a, a });
}

void OSD::px_fill(float x, float y, float w, float h, const float* c, float a) {
    a *= px_a_;
    if (a <= 0.003f || w <= 0 || h <= 0) return;
    px_vertex(x, y, c, a);     px_vertex(x + w, y, c, a);     px_vertex(x + w, y + h, c, a);
    px_vertex(x, y, c, a);     px_vertex(x + w, y + h, c, a); px_vertex(x, y + h, c, a);
}

// A convex polygon of n points (x, y pairs), as a fan.
void OSD::px_poly(const float* p, int n, const float* c, float a) {
    a *= px_a_;
    if (a <= 0.003f || n < 3) return;
    for (int i = 1; i + 1 < n; i++) {
        px_vertex(p[0], p[1], c, a);
        px_vertex(p[i * 2], p[i * 2 + 1], c, a);
        px_vertex(p[i * 2 + 2], p[i * 2 + 3], c, a);
    }
}

void OSD::px_quad(const float* p, const float* c, float a) { px_poly(p, 4, c, a); }

void OSD::px_frame(float x, float y, float w, float h, float t, const float* c, float a) {
    px_fill(x, y, w, t, c, a);
    px_fill(x, y + h - t, w, t, c, a);
    px_fill(x, y + t, t, h - 2 * t, c, a);
    px_fill(x + w - t, y + t, t, h - 2 * t, c, a);
}

// Diagonal stripes over a rectangle, each one cut to it (Sutherland-Hodgman
// against the four sides), so they go in with everything else.
void OSD::px_hatch(float x, float y, float w, float h, const float* c, float a, float pitch, float stripe) {
    if (a * px_a_ <= 0.003f || w <= 0 || h <= 0 || pitch <= 0) return;
    const float x1 = x + w, y1 = y + h;
    for (float k = -h; k < w; k += pitch) {
        float poly[2][24];
        int n = 4;
        const float s0[8] = { x + k, y1,  x + k + stripe, y1,  x + k + stripe + h, y,  x + k + h, y };
        for (int i = 0; i < 8; i++) poly[0][i] = s0[i];
        int cur = 0;
        for (int side = 0; side < 4 && n >= 3; side++) {
            auto inside = [&](float px, float py) {
                return side == 0 ? px >= x : side == 1 ? px <= x1 : side == 2 ? py >= y : py <= y1;
            };
            auto cut = [&](float ax, float ay, float bx, float by, float& ox, float& oy) {
                float t;
                if (side <= 1) { const float e = side == 0 ? x : x1; t = (e - ax) / (bx - ax); }
                else           { const float e = side == 2 ? y : y1; t = (e - ay) / (by - ay); }
                ox = ax + (bx - ax) * t; oy = ay + (by - ay) * t;
            };
            const float* in = poly[cur];
            float* out = poly[cur ^ 1];
            int m = 0;
            for (int i = 0; i < n && m < 11; i++) {
                const float ax = in[i * 2], ay = in[i * 2 + 1];
                const float bx = in[((i + 1) % n) * 2], by = in[((i + 1) % n) * 2 + 1];
                const bool ain = inside(ax, ay), bin = inside(bx, by);
                if (ain) { out[m * 2] = ax; out[m * 2 + 1] = ay; m++; }
                if (ain != bin && m < 11) { cut(ax, ay, bx, by, out[m * 2], out[m * 2 + 1]); m++; }
            }
            n = m;
            cur ^= 1;
        }
        if (n >= 3) px_poly(poly[cur], n, c, a);
    }
}

float OSD::px_text_w(const char* s, float font_px) {
    return canvas_->text_width(s, font_px * 1.6f);   // the texture is drawn at 40 px in 64
}

// align: 0 left of x, 1 centred on it, 2 right of it. `baseline` is where the
// letters sit. Returns the width drawn.
float OSD::px_text(const char* s, float x, float baseline, float font_px, int align, const float* c, float a) {
    a *= px_a_;
    if (!s || !s[0] || a <= 0.003f) return 0.0f;
    px_flush();                         // what was gathered goes under the text
    const float scale = font_px * 1.6f;
    return canvas_->text_quad(s, x, px_H_ - (baseline + 0.25f * scale), scale, align, c, a);
}

// A word on a small plate, top-left at (x, y). Outlined: a frame in bg and the
// word in bg, nothing filled. Returns the width.
float OSD::px_pill(const char* s, float x, float y, float font_px, const float* bg, const float* fg,
                   float a, bool outline) {
    const float pad = 0.5f * font_px, w = px_text_w(s, font_px) + 2 * pad, h = font_px * 1.7f;
    if (outline) px_frame(x, y, w, h, std::max(1.0f, font_px * 0.1f), bg, a);
    else         px_fill(x, y, w, h, bg, a);
    px_text(s, x + pad, y + h * 0.5f + font_px * 0.36f, font_px, 0, outline ? bg : fg, a);
    return w;
}

// The picture on screen now, drawn into a rectangle from its decoded buffer
// (import_live_picture). False, and nothing drawn, when there is none.
bool OSD::px_live(float x, float y, float w, float h) {
    if (!canvas_->video_supported() || !live_ext_tex_ || !import_live_picture()) return false;
    px_flush();
    const float gy = px_H_ - (y + h);
    const float v[] = { x, gy, 0, 0, 1,  x + w, gy, 0, 1, 1,  x, gy + h, 0, 0, 0,  x + w, gy + h, 0, 1, 0 };
    canvas_->video_quad(live_ext_tex_, v, px_mvp_, 0.0f);
    canvas_->set_transform(px_mvp_);
    return true;
}

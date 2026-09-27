#include "vec_icons.hpp"

#include <cairo/cairo.h>
#include <cmath>
#include <map>
#include <utility>

namespace {

// The design space every icon is drawn in. 32 units, chosen so the thinnest
// feature (a 2.4-unit stroke) lands on a whole pixel at the 24px the HUD
// actually asks for - below that the antialiaser has to guess, and a guessed
// edge is exactly the softness we are removing.
struct Ink { double x, y, w, h; };   // the box the strokes occupy, in design units

void path_drone(cairo_t* cr) {
    // Ring quad, seen from above: an X of arms with a motor ring on each end.
    // The arms stop short of the rings rather than running under them, so at
    // 24px the ring reads as a ring and not as a filled blob.
    cairo_set_line_width(cr, 2.6);
    cairo_move_to(cr, 10.5, 10.5); cairo_line_to(cr, 21.5, 21.5);
    cairo_move_to(cr, 21.5, 10.5); cairo_line_to(cr, 10.5, 21.5);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 2.4);
    static const double c[4][2] = { {7.5,7.5}, {24.5,7.5}, {7.5,24.5}, {24.5,24.5} };
    for (int i = 0; i < 4; i++) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, c[i][0], c[i][1], 4.2, 0.0, 2.0 * M_PI);
    }
    cairo_stroke(cr);
}

void path_goggles(cairo_t* cr) {
    // Twin lens: two rounded lenses and the bridge between them.
    cairo_set_line_width(cr, 2.4);
    for (int i = 0; i < 2; i++) {
        const double x = i ? 17.4 : 2.4, y = 10.4, w = 12.2, h = 11.2, r = 4.0;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0.0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0.0,        M_PI / 2);
        cairo_arc(cr, x + r,     y + h - r, r,  M_PI / 2,  M_PI);
        cairo_arc(cr, x + r,     y + r,     r,  M_PI,      3 * M_PI / 2);
        cairo_close_path(cr);
    }
    cairo_stroke(cr);

    cairo_move_to(cr, 14.6, 16.0); cairo_line_to(cr, 17.4, 16.0);
    cairo_stroke(cr);
}

void path_distance(cairo_t* cr) {
    // Span marker. End caps say "measured between these two points"; the
    // chevrons say which way the measurement runs.
    cairo_set_line_width(cr, 2.4);
    cairo_move_to(cr, 3.6, 10.0); cairo_line_to(cr, 3.6, 22.0);
    cairo_move_to(cr, 28.4, 10.0); cairo_line_to(cr, 28.4, 22.0);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 2.2);
    cairo_move_to(cr, 3.6, 16.0); cairo_line_to(cr, 28.4, 16.0);
    cairo_stroke(cr);

    cairo_move_to(cr, 10.6, 11.6); cairo_line_to(cr, 6.4, 16.0); cairo_line_to(cr, 10.6, 20.4);
    cairo_move_to(cr, 21.4, 11.6); cairo_line_to(cr, 25.6, 16.0); cairo_line_to(cr, 21.4, 20.4);
    cairo_stroke(cr);
}

struct Def {
    void (*path)(cairo_t*);
    Ink  ink;
};

const Def& def_for(VecIcon which) {
    // Ink boxes include half a stroke width on every side - the outline of a
    // 2.4-wide stroke centred on the path reaches 1.2 past it.
    static const Def kDrone    = { path_drone,    { 2.1,  2.1, 27.8, 27.8 } };
    static const Def kGoggles  = { path_goggles,  { 1.2,  9.2, 29.6, 13.6 } };
    static const Def kDistance = { path_distance, { 2.4,  8.8, 27.2, 14.4 } };
    switch (which) {
        case VecIcon::Goggles:  return kGoggles;
        case VecIcon::Distance: return kDistance;
        case VecIcon::Drone:
        default:                return kDrone;
    }
}

GLuint upload(cairo_surface_t* s) {
    cairo_surface_flush(s);
    const int w = cairo_image_surface_get_width(s);
    const int h = cairo_image_surface_get_height(s);
    unsigned char* data = cairo_image_surface_get_data(s);
    if (!data) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // CLAMP_TO_EDGE matters here in a way it does not for the glyph atlas: the
    // quad is sized to the texture, so a REPEAT wrap would fold the far edge's
    // antialiased pixels back over the near one.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Cairo's ARGB32 is premultiplied BGRA on little-endian, which is exactly
    // what the shader's is_text branch already swizzles and tints.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return tex;
}

struct Cached { GLuint tex; float aspect; };
std::map<std::pair<int, int>, Cached> g_cache;

}  // namespace

GLuint vec_icon_texture(VecIcon which, int px_h, float* out_aspect) {
    // ui_scale is a discrete setting, so in practice this quantisation means
    // the cache holds one entry per icon for the life of the process.
    if (px_h < 8)   px_h = 8;
    if (px_h > 512) px_h = 512;
    px_h = (px_h + 2) / 4 * 4;

    const Def& d = def_for(which);
    const float aspect = (float)(d.ink.w / d.ink.h);
    if (out_aspect) *out_aspect = aspect;

    const auto key = std::make_pair((int)which, px_h);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second.tex;

    const double scale = px_h / d.ink.h;
    const int    px_w  = (int)lround(d.ink.w * scale);

    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return 0;
    }
    cairo_t* cr = cairo_create(s);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -d.ink.x, -d.ink.y);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    // White at full alpha: draw_icon multiplies the whole texture by one RGB
    // triple, so the icon carries shape only and the HUD supplies the colour.
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    d.path(cr);
    cairo_destroy(cr);

    GLuint tex = upload(s);
    cairo_surface_destroy(s);
    if (tex) g_cache[key] = { tex, aspect };
    return tex;
}

void vec_icon_cache_clear() {
    for (auto& kv : g_cache)
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    g_cache.clear();
}

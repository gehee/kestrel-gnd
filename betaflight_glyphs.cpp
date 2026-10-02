#include "betaflight_glyphs.hpp"
#include <cmath>
#include <initializer_list>

namespace BetaflightGlyphs {

// Betaflight's symbol codes, from src/main/drivers/osd_symbols.h - the FC sends
// these as the character bytes of its DisplayPort stream, so this is the one
// table the whole OSD is read against. Only the numbering comes from there:
// every glyph below is drawn here in vectors (see draw_glyph), because the
// bitmap fonts that used to sit beside this file were dropped for want of a
// licence.
//
// Codes the header leaves out (0x77, 0x78, 0x7C) are unassigned slots in the
// font, and draw as nothing - not as a placeholder, which is what a code the
// FC never defines is not owed.
enum Sym {
    SYM_NONE                   = 0x00,
    SYM_RSSI                   = 0x01,
    SYM_AH_RIGHT               = 0x02,
    SYM_AH_LEFT                = 0x03,   // also the menu cursor (SYM_CURSOR)
    SYM_THR                    = 0x04,
    SYM_OVER_HOME              = 0x05,
    SYM_VOLT                   = 0x06,
    SYM_MAH                    = 0x07,
    SYM_STICK_SPRITE_HIGH      = 0x08,   // also SYM_GPS_DEGREE
    SYM_STICK_SPRITE_MID       = 0x09,
    SYM_STICK_SPRITE_LOW       = 0x0A,
    SYM_STICK_CENTER           = 0x0B,
    SYM_M                      = 0x0C,
    SYM_F                      = 0x0D,
    SYM_C                      = 0x0E,
    SYM_FT                     = 0x0F,
    SYM_BBLOG                  = 0x10,
    SYM_HOMEFLAG               = 0x11,
    SYM_RPM                    = 0x12,
    SYM_AH_DECORATION          = 0x13,
    SYM_ROLL                   = 0x14,
    SYM_PITCH                  = 0x15,
    SYM_STICK_VERTICAL         = 0x16,
    SYM_STICK_HORIZONTAL       = 0x17,
    SYM_HEADING_N              = 0x18,
    SYM_HEADING_S              = 0x19,
    SYM_HEADING_E              = 0x1A,
    SYM_HEADING_W              = 0x1B,
    SYM_HEADING_DIVIDED_LINE   = 0x1C,
    SYM_HEADING_LINE           = 0x1D,
    SYM_SAT_L                  = 0x1E,
    SYM_SAT_R                  = 0x1F,

    // 0x20-0x5F is ASCII text, bar the one code Betaflight takes back:
    SYM_CHECKERED_FLAG         = 0x24,

    SYM_ARROW_SOUTH            = 0x60,   // 0x60-0x6F: 16 arrows, S, then counter-
    SYM_ARROW_LAST             = 0x6F,   // clockwise through E (0x64), N, W (0x6C)
    SYM_SPEED                  = 0x70,
    SYM_TOTAL_DISTANCE         = 0x71,
    SYM_AH_CENTER_LINE         = 0x72,
    SYM_AH_CENTER              = 0x73,
    SYM_AH_CENTER_LINE_RIGHT   = 0x74,
    SYM_ARROW_SMALL_UP         = 0x75,
    SYM_ARROW_SMALL_DOWN       = 0x76,
    SYM_PREV_LAP_TIME          = 0x79,
    SYM_TEMPERATURE            = 0x7A,
    SYM_LINK_QUALITY           = 0x7B,
    SYM_KM                     = 0x7D,
    SYM_MILES                  = 0x7E,
    SYM_ALTITUDE               = 0x7F,
    SYM_AH_BAR9_0              = 0x80,   // 0x80-0x88: a horizon line at nine heights
    SYM_AH_BAR9_8              = 0x88,
    SYM_LAT                    = 0x89,
    SYM_PB_START               = 0x8A,
    SYM_PB_FULL                = 0x8B,
    SYM_PB_HALF                = 0x8C,
    SYM_PB_EMPTY               = 0x8D,
    SYM_PB_END                 = 0x8E,
    SYM_PB_CLOSE               = 0x8F,
    SYM_BATT_FULL              = 0x90,   // 0x90-0x96: FULL down to EMPTY
    SYM_BATT_EMPTY             = 0x96,
    SYM_MAIN_BATT              = 0x97,
    SYM_LON                    = 0x98,
    SYM_FTPS                   = 0x99,
    SYM_AMP                    = 0x9A,
    SYM_ON_M                   = 0x9B,
    SYM_FLY_M                  = 0x9C,
    SYM_MPH                    = 0x9D,
    SYM_KPH                    = 0x9E,
    SYM_MPS                    = 0x9F,
    SYM_LOGO_START             = 0xA0,   // 0xA0-0xFF: the boot logo, 24x4 tiles
    SYM_END_OF_FONT            = 0xFF,
};

// The unit letters and canned words (V, A, mAh, LQ, FLY...) this file draws
// as glyphs are OSD text like any other in the app, so they are set in the
// same face as the rest of it - see osd.cpp's char_tex_cache and draw_text.
// Without this they were "Sans", the one face nothing else here uses, which
// is why a unit sitting right beside its own number looked like it came from
// a different app.
static const char* kGlyphFont = "Chakra Petch SemiBold";

// Betaflight HD's glyph cell is 24x36 font pixels, and the 53x20 canvas is
// fitted to the whole frame, which makes the cell on screen the same 2:3. The
// glyph texture itself is square and gets stretched into that cell, so
// anything drawn in the texture's own units is squashed by a third - a circle
// comes out an upright oval. Icons are therefore drawn in cell units (cell())
// and the stretch turns them back into what was drawn. Text is the exception:
// it is drawn in texture units (tex()) exactly as osd.cpp's ASCII path draws
// it, so a unit letter keeps the proportions of the digits beside it.
static const double kCellW = 24.0;
static const double kCellH = 36.0;

// Stroke weights, in cell units: a stem of the ASCII digits is about 2 wide.
static const double kInk  = 2.0;
static const double kThin = 1.4;

struct Pt { double x, y; };

struct Pen {
    cairo_t* cr;
    int w, h;

    void cell() const { cairo_identity_matrix(cr); cairo_scale(cr, w / kCellW, h / kCellH); }
    void tex()  const { cairo_identity_matrix(cr); cairo_scale(cr, w / 64.0, h / 64.0); }

    void line(double x1, double y1, double x2, double y2, double wt = kInk) const {
        cairo_set_line_width(cr, wt);
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        cairo_stroke(cr);
    }
    void ring(double cx, double cy, double r, double wt = kInk) const {
        cairo_set_line_width(cr, wt);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_stroke(cr);
    }
    void disc(double cx, double cy, double r) const {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    void rect(double x, double y, double rw, double rh) const {
        cairo_rectangle(cr, x, y, rw, rh);
        cairo_fill(cr);
    }
    void frame(double x, double y, double rw, double rh, double wt = kInk) const {
        cairo_set_line_width(cr, wt);
        cairo_rectangle(cr, x, y, rw, rh);
        cairo_stroke(cr);
    }
    void poly(std::initializer_list<Pt> pts, bool fill) const {
        bool first = true;
        for (const Pt& p : pts) {
            if (first) cairo_move_to(cr, p.x, p.y); else cairo_line_to(cr, p.x, p.y);
            first = false;
        }
        cairo_close_path(cr);
        if (fill) cairo_fill(cr); else { cairo_set_line_width(cr, kInk); cairo_stroke(cr); }
    }
    // An open polyline, so a bracket's open side stays open.
    void path(std::initializer_list<Pt> pts, double wt = kInk) const {
        bool first = true;
        for (const Pt& p : pts) {
            if (first) cairo_move_to(cr, p.x, p.y); else cairo_line_to(cr, p.x, p.y);
            first = false;
        }
        cairo_set_line_width(cr, wt);
        cairo_stroke(cr);
    }
    // Angles in degrees, clockwise from +x as cairo has them (y is down).
    void arc(double cx, double cy, double r, double a0, double a1, double wt = kInk) const {
        cairo_set_line_width(cr, wt);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, a0 * M_PI / 180, a1 * M_PI / 180);
        cairo_stroke(cr);
    }
};

// An arc that ends in an arrowhead at a1; cw says which way it travels.
static void arc_arrow(const Pen& p, double cx, double cy, double r,
                      double a0, double a1, bool cw, double wt = kInk) {
    cairo_set_line_width(p.cr, wt);
    cairo_new_sub_path(p.cr);
    if (cw) cairo_arc(p.cr, cx, cy, r, a0 * M_PI / 180, a1 * M_PI / 180);
    else    cairo_arc_negative(p.cr, cx, cy, r, a0 * M_PI / 180, a1 * M_PI / 180);
    cairo_stroke(p.cr);

    double a  = a1 * M_PI / 180;
    double nx = cos(a), ny = sin(a);                 // outward from the centre
    double tx = cw ? -ny : ny, ty = cw ? nx : -nx;   // the way the arc is heading
    double ex = cx + r * nx, ey = cy + r * ny;
    p.poly({{ex + tx * 5.0, ey + ty * 5.0},
            {ex + nx * 3.4, ey + ny * 3.4},
            {ex - nx * 3.4, ey - ny * 3.4}}, true);
}

// A unit letter or word, centred in the cell, in texture units. Shrunk to fit
// when it would not: "ft/s" at the size "m/s" takes runs off both edges.
static void label(const Pen& p, const char* text, double size, double base = 45.0) {
    p.tex();
    cairo_select_font_face(p.cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(p.cr, size);
    cairo_text_extents_t e;
    cairo_text_extents(p.cr, text, &e);
    if (e.width > 60.0) {
        cairo_set_font_size(p.cr, size * 60.0 / e.width);
        cairo_text_extents(p.cr, text, &e);
    }
    cairo_move_to(p.cr, 32.0 - (e.x_bearing + e.width / 2.0), base);
    cairo_show_text(p.cr, text);
}

// A character drawn as the ASCII path in osd.cpp draws it: the compass letters
// are text on the heading tape, and have to sit in it as text does.
static void text_char(const Pen& p, char c) {
    p.tex();
    cairo_select_font_face(p.cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(p.cr, 56.0);
    cairo_text_extents_t e;
    char buf[2] = {c, 0};
    cairo_text_extents(p.cr, buf, &e);
    cairo_move_to(p.cr, 6.0 - e.x_bearing, 50.0);
    cairo_show_text(p.cr, buf);
}

// A degree mark and the unit letter that follows it (F, C). The degree ring
// used to be drawn at the same radius as a digit (6*s in a 64-unit cell) -
// next to the actual number it sat beside, that read as a bold "0" rather
// than a small superscript mark, so "31" beside it looked like "310C" /
// "310F". A real degree sign is a fraction of the letter's own size and
// rides high, not centred on the cell - both fixed here, and the letter
// itself is the same font glyph every other unit in this file uses (matching
// V/A/T's own size and baseline) rather than a hand-drawn arc, so "C" reads
// as the same C as everywhere else it appears on screen.
static void degree_unit(const Pen& p, const char* letter) {
    p.tex();
    cairo_set_line_width(p.cr, 2.0);
    cairo_new_sub_path(p.cr);
    cairo_arc(p.cr, 10, 14, 3.5, 0, 2 * M_PI);
    cairo_stroke(p.cr);
    cairo_select_font_face(p.cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(p.cr, 36.0);
    cairo_move_to(p.cr, 18, 45);
    cairo_show_text(p.cr, letter);
}

// A battery lying on its side, `level` of its 6 segments lit. Betaflight picks
// the glyph as SYM_BATT_EMPTY minus the cell-voltage level, so 0x90 is the
// full one and 0x96 the empty one (the other way round was this file's
// mistake: it read the code as a cell count and drew a 6S as "full").
static void battery(const Pen& p, int level) {
    p.cell();
    if (level > 0) {
        if (level <= 1)      cairo_set_source_rgb(p.cr, 0.984, 0.302, 0.239); // red
        else if (level <= 3) cairo_set_source_rgb(p.cr, 0.918, 0.769, 0.208); // yellow
        else                 cairo_set_source_rgb(p.cr, 0.012, 0.808, 0.643); // neon green
        const double gap = 0.7, seg = (13.5 - 5 * gap) / 6.0;
        for (int i = 0; i < level; i++)
            p.rect(4.0 + i * (seg + gap), 14.0, seg, 8.0);
    }
    cairo_set_source_rgb(p.cr, 1, 1, 1);
    p.frame(2.0, 11.5, 17.5, 13.0);        // body
    p.rect(20.5, 15.0, 2.5, 6.0);          // terminal
}

// SYM_MAIN_BATT: the battery outline with a bolt in it.
static void main_battery(const Pen& p) {
    p.cell();
    p.frame(2.0, 11.5, 17.5, 13.0);
    p.rect(20.5, 15.0, 2.5, 6.0);
    p.poly({{12.8, 13.8}, {7.2, 19.6}, {10.8, 19.6}, {9.2, 23.4}, {15.2, 17.0}, {11.6, 17.0}}, true);
}

// The 16 direction arrows. deg is clockwise from "up".
static void arrow(const Pen& p, double deg) {
    p.cell();
    cairo_save(p.cr);
    cairo_translate(p.cr, 12, 18);
    cairo_rotate(p.cr, deg * M_PI / 180);
    p.poly({{0, -11}, {7.5, -1}, {2.2, -1}, {2.2, 11}, {-2.2, 11}, {-2.2, -1}, {-7.5, -1}}, true);
    cairo_restore(p.cr);
}

// The satellite of the GPS count, two cells wide: half 0 is SYM_SAT_L, half 1
// SYM_SAT_R. Drawn once across 48 units and shown a half at a time.
static void satellite(const Pen& p, int half) {
    p.cell();
    cairo_save(p.cr);
    cairo_translate(p.cr, -24.0 * half, 0);
    p.poly({{24, 16}, {30, 22}, {24, 28}, {18, 22}}, true);            // body
    p.line(24, 16, 24, 12, kInk);                                       // mast
    p.ring(24, 9.5, 2.4, 1.8);                                          // dish
    p.line(18, 22, 15, 22, kInk);                                       // struts
    p.line(30, 22, 33, 22, kInk);
    for (int side = 0; side < 2; side++) {                              // panels
        double x = side ? 33.0 : 2.0;
        p.frame(x + 1, 17.5, 12, 9, kThin);
        p.line(x + 5, 17.5, x + 5, 26.5, kThin);
        p.line(x + 9, 17.5, x + 9, 26.5, kThin);
    }
    cairo_restore(p.cr);
}

static void rssi(const Pen& p) {
    p.cell();
    p.disc(12, 26, 2.4);
    for (double r : {6.5, 11.5, 16.5}) p.arc(12, 26, r, -135, -45, kInk);
}

static void throttle(const Pen& p) {
    p.cell();
    p.line(12, 5, 12, 31, 1.6);     // the gimbal's travel
    p.rect(5, 13, 14, 6);           // the stick
}

static void over_home(const Pen& p) {
    p.cell();
    p.poly({{12, 7}, {2.5, 17}, {21.5, 17}}, true);     // roof
    p.rect(5.5, 17, 13, 12);                            // walls
    cairo_save(p.cr);                                   // door, cut out
    cairo_set_operator(p.cr, CAIRO_OPERATOR_CLEAR);
    p.rect(10, 21.5, 4, 7.5);
    cairo_restore(p.cr);
}

static void blackbox(const Pen& p) {
    p.cell();
    p.frame(4.5, 9.5, 15, 17, kInk);            // a floppy: shell,
    p.rect(8, 9.5, 8, 5.5);                     // shutter,
    p.frame(8, 19.5, 8, 7, kThin);              // label
}

static void home_flag(const Pen& p) {
    p.cell();
    p.rect(5, 6, 2.2, 25);
    p.poly({{7, 7}, {20, 11.5}, {7, 16}}, true);
}

static void checkered_flag(const Pen& p) {
    p.cell();
    p.rect(4, 5, 2.2, 26);
    const double cw = 3.75, ch = 4.0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            if ((r + c) % 2 == 0) p.rect(6.2 + c * cw, 6 + r * ch, cw, ch);
    p.frame(6.2, 6, 4 * cw, 3 * ch, kThin);
}

static void roll(const Pen& p) {
    p.cell();
    p.line(3, 25, 21, 25, 2.2);                     // wings, seen from behind
    p.disc(12, 25, 2.8);                            // fuselage
    arc_arrow(p, 12, 25, 12.5, -160, -20, true);
}

static void pitch(const Pen& p) {
    p.cell();
    p.line(3, 18, 15, 18, 2.2);                     // fuselage, seen from the side
    p.poly({{3, 18}, {3, 11}, {7.5, 18}}, true);    // fin
    arc_arrow(p, 8, 18, 13.0, 55, -55, false);
}

static void speed(const Pen& p) {
    p.cell();
    p.arc(12, 24, 10, 180, 360);
    p.line(2, 24, 22, 24, kInk);
    p.line(12, 24, 17.5, 16.5, 1.8);                // needle
    p.disc(12, 24, 2.2);
}

static void total_distance(const Pen& p) {
    p.cell();
    p.ring(5.5, 27, 3.0, 1.8);                      // start
    p.disc(18.5, 9, 3.2);                           // end
    cairo_save(p.cr);
    double dash[2] = {2.6, 2.6};
    cairo_set_dash(p.cr, dash, 2, 0);
    p.line(7.5, 24.5, 16.5, 11.5, 1.8);
    cairo_restore(p.cr);
}

static void stopwatch(const Pen& p) {
    p.cell();
    p.ring(12, 21, 8.5);
    p.rect(9.5, 7.5, 5, 3);                         // crown
    p.line(12, 21, 12, 15.5, 1.8);
    p.line(12, 21, 16.2, 23.4, 1.8);
}

static void thermometer(const Pen& p) {
    p.cell();
    p.frame(9.2, 5.5, 5.6, 18, kInk);               // tube
    p.disc(12, 27.5, 5.6);                          // bulb
    p.line(12, 27, 12, 13, 2.0);                    // mercury
    for (double y : {9.0, 13.0, 17.0}) p.line(16.5, y, 20, y, kThin);
}

static void altitude(const Pen& p) {
    p.cell();
    p.poly({{2, 28}, {9, 12.5}, {14, 22}, {16.8, 17}, {22, 28}}, true);
}

// Arrow heads for the climb / descent marker; down is up flipped.
static void small_arrow(const Pen& p, bool up) {
    p.cell();
    if (!up) { cairo_translate(p.cr, 0, 36); cairo_scale(p.cr, 1, -1); }
    p.poly({{12, 7}, {5, 16.5}, {19, 16.5}}, true);
    p.rect(9.8, 16.5, 4.4, 12);
}

static void progress(const Pen& p, int code) {
    p.cell();
    const double top = 13.0, bot = 23.0;            // borders; the band is 12..24
    switch (code) {
        case SYM_PB_START:
            p.path({{24, top}, {2, top}, {2, bot}, {24, bot}});
            break;
        case SYM_PB_FULL:
            p.rect(0, 12, 24, 12);
            break;
        case SYM_PB_HALF:
            p.rect(0, 12, 12, 12);
            p.line(12, top, 24, top);
            p.line(12, bot, 24, bot);
            break;
        case SYM_PB_EMPTY:
            p.line(0, top, 24, top);
            p.line(0, bot, 24, bot);
            break;
        case SYM_PB_END:                            // where the filled run stops
            p.line(0, top, 24, top);
            p.line(0, bot, 24, bot);
            p.line(1, 12, 1, 24);
            break;
        case SYM_PB_CLOSE:
            p.path({{0, top}, {22, top}, {22, bot}, {0, bot}});
            break;
    }
}

// The boot logo is 96 glyphs, 24 across and 4 down, and each code is one tile
// of it: SYM_LOGO_START + row * 24 + col. Draw the whole 576x144 logo in cell
// units and let the tile's own window cut its piece out.
static void logo_tile(const Pen& p, int tile) {
    p.cell();
    cairo_translate(p.cr, -(tile % 24) * kCellW, -(tile / 24) * kCellH);

    // A quad seen from above: four motors on diagonal arms round a body. The
    // arms stop at the motor rings' edge (18 out along the diagonal, which is
    // 12.7 on each axis) rather than running on into their centres.
    const double cx = 72, cy = 68;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            p.line(cx, cy, cx + sx * 27.6, cy + sy * 27.6, 10);
            p.ring(cx + sx * 40, cy + sy * 40, 18, 8);
        }
    p.disc(cx, cy, 17);

    cairo_select_font_face(p.cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(p.cr, 90.0);
    cairo_text_extents_t e;
    cairo_text_extents(p.cr, "BETAFLIGHT", &e);
    cairo_set_font_size(p.cr, 90.0 * 392.0 / e.width);
    cairo_text_extents(p.cr, "BETAFLIGHT", &e);
    cairo_move_to(p.cr, 168.0 - e.x_bearing, 62.0 - (e.y_bearing + e.height / 2.0));
    cairo_show_text(p.cr, "BETAFLIGHT");
    p.rect(168, 98, 392, 7);
}

bool draw_glyph(cairo_t* cr, int char_idx, int width, int height) {
    // ASCII - 0x20-0x5F, which is all Betaflight's font keeps of it - is real
    // OSD text, and passes through to the caller's text path. Everything above
    // is a symbol (so a lowercase letter never reaches the screen: the 0x60
    // and up range is the arrows and the unit symbols), and the one code in
    // range that is not text is the lap-timer flag in '$'s place.
    if (char_idx >= 0x20 && char_idx <= 0x5F && char_idx != SYM_CHECKERED_FLAG)
        return false;

    // Clear background
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

    const Pen p{cr, width, height};

    if (char_idx >= SYM_LOGO_START && char_idx <= SYM_END_OF_FONT) {
        logo_tile(p, char_idx - SYM_LOGO_START);
        return true;
    }
    if (char_idx >= SYM_ARROW_SOUTH && char_idx <= SYM_ARROW_LAST) {
        arrow(p, 180.0 - 22.5 * (char_idx - SYM_ARROW_SOUTH));
        return true;
    }
    if (char_idx >= SYM_AH_BAR9_0 && char_idx <= SYM_AH_BAR9_8) {
        p.cell();
        double y = 2.0 + 4.0 * (char_idx - SYM_AH_BAR9_0);   // nine heights, 4 apart,
        p.line(0, y, kCellW, y);                              // so rows join up
        return true;
    }
    if (char_idx >= SYM_BATT_FULL && char_idx <= SYM_BATT_EMPTY) {
        battery(p, SYM_BATT_EMPTY - char_idx);
        return true;
    }

    switch (char_idx) {
        case SYM_RSSI:           rssi(p); break;
        case SYM_AH_RIGHT:       p.cell(); p.poly({{5, 18}, {19, 8}, {19, 28}}, true); break;
        case SYM_AH_LEFT:        p.cell(); p.poly({{19, 18}, {5, 8}, {5, 28}}, true); break;
        case SYM_THR:            throttle(p); break;
        case SYM_OVER_HOME:      over_home(p); break;
        case SYM_VOLT:           label(p, "V", 36.0); break;
        case SYM_MAH:            label(p, "mAh", 30.0); break;

        // The stick overlay's sprite at three heights; the top one doubles as
        // the degree sign of a GPS coordinate.
        case SYM_STICK_SPRITE_HIGH: p.cell(); p.ring(12, 8, 3.8, 1.8); break;
        case SYM_STICK_SPRITE_MID:  p.cell(); p.ring(12, 18, 3.8, 1.8); break;
        case SYM_STICK_SPRITE_LOW:  p.cell(); p.ring(12, 28, 3.8, 1.8); break;
        case SYM_STICK_CENTER:      p.cell(); p.line(7, 18, 17, 18, 1.6); p.line(12, 13, 12, 23, 1.6); break;
        case SYM_STICK_VERTICAL:    p.cell(); p.line(12, 0, 12, kCellH, kThin); break;
        case SYM_STICK_HORIZONTAL:  p.cell(); p.line(0, 18, kCellW, 18, kThin); break;

        case SYM_M:              label(p, "m", 44.0, 48.0); break;
        case SYM_F:              degree_unit(p, "F"); break;
        case SYM_C:              degree_unit(p, "C"); break;
        case SYM_FT:             label(p, "ft", 34.0); break;
        case SYM_BBLOG:          blackbox(p); break;
        case SYM_HOMEFLAG:       home_flag(p); break;
        case SYM_RPM:            label(p, "RPM", 28.0); break;
        case SYM_ROLL:           roll(p); break;
        case SYM_PITCH:          pitch(p); break;

        // The sidebar tick: a vertical rule with a notch, which joins itself
        // into a ladder when stacked.
        case SYM_AH_DECORATION:
            p.cell(); p.line(12, 0, 12, kCellH); p.line(7, 18, 17, 18, kThin); break;

        // The compass tape: its four letters are text, the ticks between them
        // are tall (the 45 degree marks) and short.
        case SYM_HEADING_N:      text_char(p, 'N'); break;
        case SYM_HEADING_S:      text_char(p, 'S'); break;
        case SYM_HEADING_E:      text_char(p, 'E'); break;
        case SYM_HEADING_W:      text_char(p, 'W'); break;
        case SYM_HEADING_DIVIDED_LINE: p.cell(); p.line(12, 12, 12, 32); break;
        case SYM_HEADING_LINE:         p.cell(); p.line(12, 22, 12, 32); break;

        case SYM_SAT_L:          satellite(p, 0); break;
        case SYM_SAT_R:          satellite(p, 1); break;
        case SYM_CHECKERED_FLAG: checkered_flag(p); break;

        case SYM_SPEED:          speed(p); break;
        case SYM_TOTAL_DISTANCE: total_distance(p); break;

        // The crosshair is three cells: a wing, the ring, a wing. The wings
        // run out to the ring's cell, and the ring's own stubs back to them.
        case SYM_AH_CENTER_LINE:
            p.cell(); p.line(2, 18, kCellW, 18); break;
        case SYM_AH_CENTER:
            p.cell(); p.ring(12, 18, 4.0); p.line(0, 18, 8, 18); p.line(16, 18, kCellW, 18); break;
        case SYM_AH_CENTER_LINE_RIGHT:
            p.cell(); p.line(0, 18, 22, 18); break;

        case SYM_ARROW_SMALL_UP:   small_arrow(p, true); break;
        case SYM_ARROW_SMALL_DOWN: small_arrow(p, false); break;
        case SYM_PREV_LAP_TIME:    stopwatch(p); break;
        case SYM_TEMPERATURE:      thermometer(p); break;
        case SYM_LINK_QUALITY:     label(p, "LQ", 36.0); break;
        case SYM_KM:               label(p, "km", 34.0); break;
        case SYM_MILES:            label(p, "mi", 34.0); break;
        case SYM_ALTITUDE:         altitude(p); break;

        case SYM_LAT:              label(p, "LAT", 28.0); break;
        case SYM_LON:              label(p, "LON", 28.0); break;

        case SYM_PB_START: case SYM_PB_FULL: case SYM_PB_HALF:
        case SYM_PB_EMPTY: case SYM_PB_END:  case SYM_PB_CLOSE:
            progress(p, char_idx);
            break;

        case SYM_MAIN_BATT:      main_battery(p); break;
        case SYM_FTPS:           label(p, "ft/s", 26.0); break;
        case SYM_AMP:            label(p, "A", 36.0); break;
        case SYM_ON_M:           label(p, "ON", 30.0); break;
        case SYM_FLY_M:          label(p, "FLY", 30.0); break;
        case SYM_MPH:            label(p, "mph", 30.0); break;
        case SYM_KPH:            label(p, "kmh", 30.0); break;
        case SYM_MPS:            label(p, "m/s", 28.0); break;

        default:
            // SYM_NONE, the slots the header leaves unassigned (0x77, 0x78,
            // 0x7C), and anything past a byte: blank, as the font itself has.
            break;
    }
    return true;
}

} // namespace BetaflightGlyphs

#include "betaflight_glyphs.hpp"
#include <cmath>

namespace BetaflightGlyphs {

// The unit letters and canned words (V, A, mAh, LQ, FLY...) this file draws
// as glyphs are OSD text like any other in the app, so they are set in the
// same face as the rest of it - see osd.cpp's char_tex_cache and draw_text.
// Without this they were "Sans", the one face nothing else here uses, which
// is why a unit sitting right beside its own number looked like it came from
// a different app.
static const char* kGlyphFont = "Chakra Petch SemiBold";

// Helper to draw a filled battery icon
static void draw_battery(cairo_t* cr, int w, int h, int cells, bool fill_enabled, int char_idx) {
    double scale_x = w / 64.0;
    double scale_y = h / 64.0;
    
    int level = (char_idx >= 0x90 && char_idx <= 0x96) ? (char_idx - 0x90) : 0;

    // 1. Fill (if level > 0)
    if (level > 0) {
        if (level <= 1) { // 0x91 (Almost empty)
            cairo_set_source_rgb(cr, 0.984, 0.302, 0.239); // Red
        } else if (level <= 3) { // 0x92, 0x93 (Mid)
            cairo_set_source_rgb(cr, 0.918, 0.769, 0.208); // Yellow
        } else { // 0x94, 0x95, 0x96 (High/Full)
            cairo_set_source_rgb(cr, 0.012, 0.808, 0.643); // Neon Green
        }
        
        double fill_w = (32.0 * level) / 6.0;
        cairo_rectangle(cr, 12 * scale_x, 24 * scale_y, fill_w * scale_x, 16 * scale_y);
        cairo_fill(cr);
    }

    // 2. Outline (always white)
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 3.0 * scale_x);
    
    // Battery outline
    cairo_rectangle(cr, 8 * scale_x, 20 * scale_y, 40 * scale_x, 24 * scale_y);
    cairo_stroke(cr);
    
    // Battery terminal
    cairo_rectangle(cr, 48 * scale_x, 28 * scale_y, 6 * scale_x, 8 * scale_y);
    cairo_fill(cr);
    
    // Cell dividers
    if (cells > 1) {
        cairo_set_line_width(cr, 1.5 * scale_x);
        for (int i = 1; i < cells && i < 7; i++) {
            double x = 8 + (40.0 / cells) * i;
            cairo_move_to(cr, x * scale_x, 20 * scale_y);
            cairo_line_to(cr, x * scale_x, 44 * scale_y);
            cairo_stroke(cr);
        }
    }
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
static void draw_degree_unit(cairo_t* cr, double s, const char* letter) {
    cairo_set_line_width(cr, 2.0 * s);
    cairo_arc(cr, 10*s, 14*s, 3.5*s, 0, 2*M_PI);
    cairo_stroke(cr);
    cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 36.0 * s);
    cairo_move_to(cr, 18*s, 45*s);
    cairo_show_text(cr, letter);
}

// Helper to draw RSSI/signal bars
static void draw_signal_bars(cairo_t* cr, int w, int h, int bars) {
    double scale_x = w / 64.0;
    double scale_y = h / 64.0;
    
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    
    for (int i = 0; i < 5; i++) {
        double bar_h = 10 + i * 8;
        double x = 10 + i * 10;
        
        if (i < bars) {
            cairo_rectangle(cr, x * scale_x, (54 - bar_h) * scale_y, 
                          7 * scale_x, bar_h * scale_y);
            cairo_fill(cr);
        } else {
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, x * scale_x, (54 - bar_h) * scale_y, 
                          7 * scale_x, bar_h * scale_y);
            cairo_stroke(cr);
        }
    }
}

// Helper to draw a modern GPS icon
static void draw_gps_icon(cairo_t* cr, int w, int h) {
    double scale_x = w / 64.0;
    double scale_y = h / 64.0;
    double cx = 32 * scale_x;
    double cy = 32 * scale_y;
    
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 3.0 * scale_x);
    
    // Satellite dish (bowl)
    cairo_arc(cr, cx, cy + 5*scale_y, 18 * scale_x, 0, M_PI);
    cairo_stroke(cr);
    
    // Center post
    cairo_move_to(cr, cx, cy + 5*scale_y);
    cairo_line_to(cr, cx, cy - 10*scale_y);
    cairo_stroke(cr);
    
    // Signal waves
    cairo_set_line_width(cr, 2.0 * scale_x);
    for (int i = 1; i <= 3; i++) {
        cairo_arc(cr, cx, cy - 10 * scale_y, i * 8 * scale_x, 
                 -M_PI * 0.75, -M_PI * 0.25);
        cairo_stroke(cr);
    }
}

// Helper to draw a home flag/house
static void draw_home_icon(cairo_t* cr, int w, int h) {
    double s = w / 64.0;
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    
    // Roof
    cairo_move_to(cr, 32*s, 10*s);
    cairo_line_to(cr, 10*s, 30*s);
    cairo_line_to(cr, 54*s, 30*s);
    cairo_fill(cr);
    
    // Body
    cairo_rectangle(cr, 18*s, 30*s, 28*s, 24*s);
    cairo_fill(cr);
    
    // Door
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_rectangle(cr, 28*s, 40*s, 8*s, 14*s);
    cairo_fill(cr);
}

// Directional arrow
static void draw_direction_arrow(cairo_t* cr, int w, int h, double angle_deg) {
    double s = w / 64.0;
    double cx = 32*s;
    double cy = 32*s;
    double angle_rad = (angle_deg - 90.0) * M_PI / 180.0; // Orient UP = 0 deg
    
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, angle_rad);
    
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 0, -20*s);  // Tip
    cairo_line_to(cr, -15*s, 15*s); // Bottom left
    cairo_line_to(cr, 0, 5*s);      // Inset
    cairo_line_to(cr, 15*s, 15*s);  // Bottom right
    cairo_close_path(cr);
    cairo_fill(cr);
    
    cairo_restore(cr);
}

bool draw_glyph(cairo_t* cr, int char_idx, int width, int height) {
    // Return false for standard ASCII (let Cairo handle it) - except the
    // handful of codes Betaflight overloads as icon markers despite them
    // landing in the printable range: the four direction arrows and the
    // alternate link-quality marker (BF_SYM_LQ_ALT in msp_osd.cpp's own
    // scraper - confirmed live as the '{' immediately before "NN:NN" LQ
    // readouts, e.g. "{27:100"). Every other printable character is real
    // OSD text and passes through exactly as before.
    if (char_idx >= 32 && char_idx <= 126 &&
        !(char_idx >= 0x77 && char_idx <= 0x7B)) {
        return false;
    }

    // Clear background
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    
    double s = width / 64.0;
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 3.0 * s);
    
    switch (char_idx) {
        // --- 0x00-0x1F range symbols ---
        case 0x01: // SYM_RSSI
            draw_signal_bars(cr, width, height, 5);
            return true;
        case 0x02: // SYM_LINK_QUALITY
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 10*s, 45*s);
            cairo_show_text(cr, "LQ");
            return true;
        case 0x03: // SYM_WARNING
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 40.0 * s);
            cairo_move_to(cr, 22*s, 45*s);
            cairo_show_text(cr, "!");
            return true;
        case 0x04: // SYM_THR (T icon)
            cairo_rectangle(cr, 15*s, 15*s, 34*s, 8*s); // Top
            cairo_rectangle(cr, 28*s, 15*s, 8*s, 34*s); // Stem
            cairo_fill(cr);
            return true;
        case 0x05: // SYM_DISARMED
            return false; // Let OSD text handle it
        case 0x06: // SYM_VOLT (V icon)
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "V");
            return true;
        case 0x07: // SYM_MAH (mAh text)
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 30.0 * s);
            cairo_move_to(cr, 2*s, 45*s);
            cairo_show_text(cr, "mAh");
            return true;
        case 0x08: // SYM_AMP (A text)
        case 0x9A: // Amps (A text)
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "A");
            return true;
        case 0x09: // SYM_FLYMODE
            return false;
        case 0x0A: // SYM_KMH
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 30.0 * s);
            cairo_move_to(cr, 2*s, 45*s);
            cairo_show_text(cr, "kmh");
            return true;
        case 0x0B: // SYM_MPH
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 30.0 * s);
            cairo_move_to(cr, 2*s, 45*s);
            cairo_show_text(cr, "mph");
            return true;
        case 0x0C: // SYM_ALT (meters icon)
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 44.0 * s);
            cairo_move_to(cr, 10*s, 48*s);
            cairo_show_text(cr, "m");
            return true;
        case 0x0D: // SYM_F (Degree Fahrenheit)
            draw_degree_unit(cr, s, "F");
            return true;
        case 0x7F: // SYM_ALT (Altitude Header/Icon)
            cairo_set_line_width(cr, 4.0 * s);
            cairo_move_to(cr, 10*s, 50*s);
            cairo_line_to(cr, 32*s, 15*s);
            cairo_line_to(cr, 54*s, 50*s);
            cairo_stroke(cr);
            return true;
        case 0x1D: // SYM_TEMPERATURE
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "T");
            return true;
        case 0x0E: // SYM_C (Degree Celsius)
            draw_degree_unit(cr, s, "C");
            return true;
        case 0x0F: // SYM_ON_TIME
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 30.0 * s);
            cairo_move_to(cr, 10*s, 45*s);
            cairo_show_text(cr, "ON");
            return true;
        case 0x10: // SYM_FLY_TIME
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 30.0 * s);
            cairo_move_to(cr, 2*s, 45*s);
            cairo_show_text(cr, "FLY");
            return true;
        case 0x11: // SYM_RTC_TIME
            // Clock symbol or just RTC
            cairo_arc(cr, 32*s, 32*s, 20*s, 0, 2*M_PI);
            cairo_stroke(cr);
            cairo_move_to(cr, 32*s, 32*s);
            cairo_line_to(cr, 32*s, 20*s);
            cairo_move_to(cr, 32*s, 32*s);
            cairo_line_to(cr, 44*s, 32*s);
            cairo_stroke(cr);
            return true;
        case 0x12: // SYM_RPM
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 28.0 * s);
            cairo_move_to(cr, 2*s, 45*s);
            cairo_show_text(cr, "RPM");
            return true;
        case 0x13: // SYM_ROLL
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "R");
            return true;
        case 0x14: // SYM_PITCH
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "P");
            return true;
        case 0x15: // SYM_YAW
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "Y");
            return true;
        case 0x16: // SYM_MAIN_BATT
            draw_battery(cr, width, height, 1, false, 0x90);
            return true;
        case 0x19: // SYM_GPS_SATS
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 10*s, 45*s);
            cairo_show_text(cr, "S");
            return true;
        case 0x1C: // SYM_POWER
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 15*s, 45*s);
            cairo_show_text(cr, "W");
            return true;
        case 0x1A: // SYM_DIST
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "D");
            return true;
        case 0x1B: // SYM_HOME
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 20*s, 45*s);
            cairo_show_text(cr, "H");
            return true;
        case 0x1E: // SYM_SAT_L
        case 0x1F: // SYM_SAT_R
        case 0x18: // SYM_SATELLITE
            draw_gps_icon(cr, width, height);
            return true;
            
        // --- Navigation Arrows (0x77-0x7A) ---
        case 0x77: // UP
            cairo_move_to(cr, 32*s, 15*s); cairo_line_to(cr, 15*s, 45*s); cairo_line_to(cr, 49*s, 45*s); cairo_close_path(cr);
            cairo_fill(cr); return true;
        case 0x78: // DOWN
            cairo_move_to(cr, 32*s, 45*s); cairo_line_to(cr, 15*s, 15*s); cairo_line_to(cr, 49*s, 15*s); cairo_close_path(cr);
            cairo_fill(cr); return true;
        case 0x79: // LEFT
            cairo_move_to(cr, 15*s, 32*s); cairo_line_to(cr, 45*s, 15*s); cairo_line_to(cr, 45*s, 49*s); cairo_close_path(cr);
            cairo_fill(cr); return true;
        case 0x7A: // RIGHT
            cairo_move_to(cr, 45*s, 32*s); cairo_line_to(cr, 15*s, 15*s); cairo_line_to(cr, 15*s, 49*s); cairo_close_path(cr);
            cairo_fill(cr); return true;

        case 0x7B: // SYM_LQ_ALT - a second link-quality marker (see
                   // BF_SYM_LQ_ALT in msp_osd.cpp's own scraper, which
                   // already treated 0x7B this way; this file had it down as
                   // SYM_TOTAL_MAH, which was never reachable to disagree -
                   // it fell into the ASCII passthrough as a literal '{'
                   // until the guard above carved this range out). Same
                   // glyph as 0x02, not a second word, so the eye reads one
                   // meaning for LQ wherever it appears.
            cairo_select_font_face(cr, kGlyphFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 36.0 * s);
            cairo_move_to(cr, 10*s, 45*s);
            cairo_show_text(cr, "LQ");
            return true;

        // --- RSSI Bars (0x80-0x86) ---
        case 0x80: draw_signal_bars(cr, width, height, 0); return true;
        case 0x81: draw_signal_bars(cr, width, height, 1); return true;
        case 0x82: draw_signal_bars(cr, width, height, 2); return true;
        case 0x83: draw_signal_bars(cr, width, height, 3); return true;
        case 0x84: draw_signal_bars(cr, width, height, 4); return true;
        case 0x85: draw_signal_bars(cr, width, height, 5); return true;
            
        // --- Battery Evolution (0x90-0x9F range) ---
        case 0x90: draw_battery(cr, width, height, 1, false, char_idx); return true; // Empty
        case 0x91: draw_battery(cr, width, height, 1, true, char_idx); return true;  // 1S
        case 0x92: draw_battery(cr, width, height, 2, true, char_idx); return true;  // 2S
        case 0x93: draw_battery(cr, width, height, 3, true, char_idx); return true;  // 3S
        case 0x94: draw_battery(cr, width, height, 4, true, char_idx); return true;  // 4S
        case 0x95: draw_battery(cr, width, height, 5, true, char_idx); return true;  // 5S
        case 0x96: draw_battery(cr, width, height, 6, true, char_idx); return true;  // 6S
        
        // --- Units ---
        case 0x57: // 'W' - already ASCII, but in BF it's often SYM_WATT
            return false; 

        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            // Compass arrows - use simple dot or arrow
            cairo_arc(cr, 32*s, 32*s, 10*s, 0, 2*M_PI);
            cairo_fill(cr);
            return true;

        default:
            // Placeholder box for unknown non-ASCII
            cairo_rectangle(cr, 10*s, 10*s, 44*s, 44*s);
            cairo_stroke(cr);
            return true;
    }
}

} // namespace BetaflightGlyphs

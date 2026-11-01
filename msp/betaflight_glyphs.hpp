#ifndef BETAFLIGHT_GLYPHS_HPP
#define BETAFLIGHT_GLYPHS_HPP

#include <cairo.h>

// Betaflight OSD Character Map
// Based on https://www.betaflight.com/docs/development/OSD-Glyps

namespace BetaflightGlyphs {

// Draw a Betaflight OSD glyph using Cairo
// Returns true if the glyph was drawn, false if it's a standard ASCII character
bool draw_glyph(cairo_t* cr, int char_idx, int width, int height);

} // namespace BetaflightGlyphs

#endif // BETAFLIGHT_GLYPHS_HPP

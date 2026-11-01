// The flight controller's own OSD: Betaflight's 53x20 canvas over the
// picture (HUD > Betaflight OSD), each cell a glyph or a letter.
#include "hud/osd_detail.hpp"

void OSD::draw_betaflight(Frame& f) {
    auto& screen_w = f.screen_w;
    auto& screen_h = f.screen_h;
    auto& mvp_hud_c = f.mvp_hud_c;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& is_menu_open = f.is_menu_open;
    auto& canopy_on = f.canopy_on;
    // --- MSP OSD ---
    // Betaflight's canvas was always drawn flat white, the one thing on
    // screen a theme change did nothing to. Every glyph here is white on
    // transparent already (BetaflightGlyphs::draw_glyph, and the ASCII path
    // below) bar the battery icon's own fill colours, so tinting the whole
    // canvas in the theme's text role - the same role every other reading
    // uses - carries it along with everything else without a second table
    // of which glyph is which.
    const HudTheme& msp_theme = hud_theme_current();
    auto draw_msp_span_func = [&](float x, float y, const std::vector<uint16_t>& span) {
        float cw, ch;
        msp_osd.get_cell_size(cw, ch);

        // Each cell's texture is rendered at the size it is drawn at, on screen: a 64x64
        // texture squashed into a cell 36x54 (a third narrower than tall) made the letters
        // condensed and soft, next to the canopy's, which are drawn at their own pixel size.
        // The canvas spans the picture, so a cell is the picture's pixels over the grid.
        const float pic = dev ? dev->picture_scale() / 100.0f : 1.0f;
        const float px_per_unit_x = (float)screen_w * pic / (2.0f * frustum_w);
        const float px_per_unit_y = (float)screen_h * pic / (2.0f * frustum_h);
        const int tw = std::max(8, (int)lroundf(cw * px_per_unit_x));
        const int th = std::max(8, (int)lroundf(ch * px_per_unit_y));
        if (tw != char_tex_w_ || th != char_tex_h_) {      // a new size: draw them all again
            for (int i = 0; i < 1024; i++)
                if (char_tex_cache[i]) { canvas_->free_image(char_tex_cache[i]); char_tex_cache[i] = 0; }
            char_tex_w_ = tw;
            char_tex_h_ = th;
            for (int i = 0; i < 1024; i++) { char_w_px_[i] = 0; char_adv_px_[i] = 0; }
        }

        auto ensure = [&](uint16_t char_idx) {
            if (char_adv_px_[char_idx] != 0) return;           // done (a blank one has an advance too)
            // The glyph table first, for every char_idx, not just the ones outside 32-126: a few
            // reserved codes Betaflight uses as icon markers (0x77-0x7B - arrows, link quality)
            // land inside that range. draw_glyph knows which in-range codes are really reserved
            // and declines (returns false) for everything else, so it is the one place that decides.
            // An icon is a whole cell wide. Text is as wide as the letter itself.
            double adv = 0;
            int w = tw;
            cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
            cairo_t *pc = cairo_create(probe);
            const bool icon = BetaflightGlyphs::draw_glyph(pc, char_idx, tw, th);
            cairo_destroy(pc);
            cairo_surface_destroy(probe);
            cairo_text_extents_t extents;
            char buf[2] = {(char)char_idx, 0};
            if (icon) {
                adv = tw;
            } else {
                // Text: the canopy's face, sized to the cell's height and never stretched, spaced
                // as the rest of the HUD spaces it - each letter its own advance, a space its own
                // width - not one cell apiece.
                cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
                cairo_t *mc = cairo_create(m);
                cairo_select_font_face(mc, "Chakra Petch SemiBold", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(mc, 0.84 * th);
                cairo_text_extents(mc, buf, &extents);
                cairo_destroy(mc);
                cairo_surface_destroy(m);
                adv = extents.x_advance;
                if (char_idx == ' ') adv = std::max(adv, 0.30 * th);
                w = std::max(4, (int)std::ceil(std::max(adv, extents.x_bearing + extents.width)) + 3);
            }
            char_w_px_[char_idx] = w;
            char_adv_px_[char_idx] = (float)std::max(adv, 1.0);
            if (char_idx == ' ') return;                        // nothing to draw

            cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, th);
            cairo_t *cr = cairo_create(surface);
            if (icon) {
                BetaflightGlyphs::draw_glyph(cr, char_idx, tw, th);
            } else {
                cairo_select_font_face(cr, "Chakra Petch SemiBold", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, 0.84 * th);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
                cairo_move_to(cr, 1.0, 0.78 * th);              // the pen, as in any line of text
                cairo_show_text(cr, buf);
            }
            unsigned char* data = cairo_image_surface_get_data(surface);
            render::ImageId tex;
            tex = canvas_->make_image(w, th, data);
            char_tex_cache[char_idx] = tex;
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
        };

        // A soft drop shadow, as the canopy's type has, so it reads on bright video; the whole
        // span's shadows first, so a glyph's shadow never lies over its neighbour. Then the
        // glyphs, slightly dimmed like the canopy's.
        const float sx = 1.5f / px_per_unit_x, sy = 1.5f / px_per_unit_y;
        for (int pass = 0; pass < 2; pass++) {
            float pen_px = 0;                                    // along the run, pixels
            for (uint16_t char_idx : span) {
                if (char_idx >= 1024) { pen_px += tw; continue; }
                ensure(char_idx);
                const float cx = x + pen_px / px_per_unit_x;
                if (char_tex_cache[char_idx] != 0) {
                    const float qw = char_w_px_[char_idx] / px_per_unit_x;
                    if (pass == 0)
                        canvas_->icon(char_tex_cache[char_idx], cx + sx, y - sy, qw, ch, 0.0f, 0.0f, 0.0f, 0.55f);
                    else
                        canvas_->icon(char_tex_cache[char_idx], cx, y, qw, ch,
                                  msp_theme.text[0], msp_theme.text[1], msp_theme.text[2], 0.92f);
                }
                pen_px += char_adv_px_[char_idx];
            }
        }
    };

    // Full 53x20 Betaflight canvas, centred and fitted to the visible area.
    // Drawn on the centre matrix so it stays flat: the side panels are rotated
    // into perspective, which is wrong for a character grid the pilot reads.
    // With the canopy up, the elements whose values it shows read off this
    // canvas are left blank, so a figure is never on screen twice: the
    // voltage it shows (cell or pack, HUD > Canopy Voltage), current, flight
    // mode, speed, Link Quality, DISARMED, and armed the timer it counts. A
    // value the air unit got from the flight controller over MSP instead
    // leaves its element drawn, as do RSSI, altitude, attitude and the rest.
    // With the canopy off the canvas is drawn whole - it is the whole picture
    // then.
    //
    // Never while the canopy is awake: its unfolded blades are meant to be
    // the topmost thing on screen, but this canvas is 53x20
    // cells of dense text with no gaps to read through - over the menu it
    // just looked like noise laid across it. The FC's own OSD is still
    // running underneath; it comes back the moment the menu closes.
    if (bf_osd && !is_menu_open) {
        canvas_->set_transform(mvp_hud_c);

        const float avail_w = 2.0f * frustum_w;
        const float avail_h = 2.0f * frustum_h;
        const int   gw = MspOsd::grid_w();
        const int   gh = MspOsd::grid_h();

        // Independently, not aspect-locked: Betaflight's HD canvas is
        // defined to map its 53x20 grid onto the WHOLE video frame, one
        // axis at a time, not onto a fixed-aspect block centred in it. The
        // previous line kept the aspect ratio the old side-panel layout
        // used (0.045 / 0.03375, tuned for a narrow strip down each side)
        // and shrank whichever axis ran out first to preserve it. On this
        // screen's 16:9 the width branch was already exact, but the height
        // it derived from that aspect came out to ~89% of the frame - a gap
        // close to a full row's height at both the top and the bottom - so
        // anything near row 0 or row 19 landed inside that margin instead
        // of on the physical edge, however precisely its column was placed.
        // Dividing each axis by its own cell count fills the frame exactly,
        // in whatever aspect that produces per cell - the one the real
        // canvas has - so both ends of both axes land on the true edges.
        const float cw = avail_w / (float)gw;
        const float ch = avail_h / (float)gh;

        msp_osd.set_cell_size(cw, ch);
        // draw_region()'s y is the first row's baseline and rows descend from
        // it, so start one cell below the top of the fitted block.
        unsigned hide = 0;
        if (canopy_on) {
            hide = (volt_mode == 1 ? BfTelem::kElPackV : BfTelem::kElCellV) | BfTelem::kElAmps |
                   BfTelem::kElMode | BfTelem::kElSpeed | BfTelem::kElLq | BfTelem::kElArm;
            if (canopy_arm_prev_ == 1) hide |= BfTelem::kElTimer;   // the canopy's timer slot, armed
            // Only where the canopy's value is that element's: one the flight
            // controller sends over MSP leaves its element on the canvas.
            hide &= msp_osd.get_telem().osd_src;
        }
        msp_osd.draw_region(0, 0, gh, gw,
                            -((float)gw * cw) / 2.0f,
                            ((float)gh * ch) / 2.0f - ch,
                            draw_msp_span_func, hide);
    }

    prof::mark(prof::kBfGrid);
}

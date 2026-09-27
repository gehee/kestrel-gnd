// The settings menu, as three columns.
//
// Section, setting, value. Focus walks rightwards into them: the column you are
// in is lit, the ones behind it dim but keep their selection, so the path you
// took is on screen without a breadcrumb to read.
//
// Two things this shape buys.
//
// Left and right stop being overloaded. The old menu spent them on nudging a
// value, which meant the same key did different things depending on which row
// you were on; here right always goes deeper and left always comes back, and
// up/down always moves within whatever column has focus. With four buttons and
// no pointer that consistency is worth more than the keystroke it costs.
//
// And it leaves the HUD alone. The canopy puts its instruments at the two
// screen edges and keeps the middle for video, so a menu in the middle covers
// the one thing it is allowed to. Both blades stay lit and stay live, which is
// what makes it possible to watch Theme or UI Scaling take effect while you are
// still on the row that changes them - the old full-width slab could only be
// backed out of and re-entered to see what it had done.

#include "osd.hpp"
#include "hud_theme.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// A ceiling on rows per column. The real count is worked out from the box
// height at draw time, because the box height depends on where the blades
// are, which depends on the UI scale.
constexpr int kVisibleMax = 12;

constexpr const char* kTabNames[6] = { "VIDEO", "AR8030", "HUD", "DISPLAY", "DVR", "SYSTEM" };

// Keep the selection in the middle of the window where the list is long enough
// to allow it, and pinned at the ends where it is not.
int window_start(int sel, int count, int visible) {
    if (count <= visible) return 0;
    int half = (visible - 1) / 2;
    int start = sel - half;
    if (start < 0) start = 0;
    if (start > count - visible) start = count - visible;
    return start;
}

// "< 5px > *" reads as "5px *", or as "5px" where the mark does not belong.
//
// The chevrons told you to press left/right on this row; left and right now
// move between columns, so they would be a lie. The pending mark rides on the
// end, and has to be lifted off first or the chevron test cannot see the
// closing one underneath it.
void tidy_value(char* t, bool keep_mark) {
    size_t n = strlen(t);
    bool marked = (n >= 2 && t[n-1] == '*' && t[n-2] == ' ');
    if (marked) { t[n-2] = '\0'; n -= 2; }
    if (n >= 4 && t[0] == '<' && t[1] == ' ' && t[n-2] == ' ' && t[n-1] == '>') {
        memmove(t, t + 2, n - 4);
        t[n - 4] = '\0';
        n -= 4;
    }
    if (marked && keep_mark) { t[n] = ' '; t[n+1] = '*'; t[n+2] = '\0'; }
}

}  // namespace

void OSD::draw_menu_columns(math::Mat4& mvp, float fw, float fh) {
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp);

    const HudTheme& T = hud_theme_current();
    const float s = osd_vars.ui_scale;

    // The box. Centred, and deliberately short of the blades: they start about
    // 0.62 of the way out on each side, so 0.58 leaves them clear.
    const float bw = 1.16f * fw;
    const float bx = -bw * 0.5f;
    // The floor is where the blades start, not a number chosen once at 1.0x.
    // The canopy is laid out in mockup pixels scaled by the UI setting, so its
    // top rail climbs as that goes up - a menu with a fixed margin overlapped
    // the panels the moment anyone scaled the HUD.
    // The clearance over the rail is what keeps the DISARMED and IDLE marks
    // legible under the menu's bottom edge: at 0.05 the box sat right on top
    // of them and the two read as one block.
    const float floor_y = canopy_top_y_ + 0.14f * fh;   // clear of the rail
    // Grow downwards from a fixed top edge, and stop at the floor. The top
    // edge keeps a margin for the clock strip and nothing else, so the menu
    // spends the room it has rather than leaving a third of the screen empty
    // above it.
    const float top_y = 0.78f * fh;
    float bh = top_y - floor_y;
    // Never so short that the rows are unreadable: below this the menu keeps
    // its height and the blades are the thing that gives way, since the pilot
    // is looking at the menu.
    const float bh_min = 0.72f * fh;
    if (bh < bh_min) bh = bh_min;
    const float by = top_y - bh;

    const float colw[3] = { bw * 0.24f, bw * 0.38f, bw * 0.38f };
    float colx[3];
    colx[0] = bx;
    colx[1] = colx[0] + colw[0];
    colx[2] = colx[1] + colw[1];

    const float pad     = 0.018f * s;
    const float head_h  = 0.085f * s;
    const float desc_h  = 0.095f * s;
    // How many rows actually fit, rather than a constant that assumed a height.
    // Sized for reading through the optics, not for fitting the most rows.
    // This started at 0.072, which held twelve rows and none of them were
    // comfortable next to the HUD's own type; the panels set the expectation
    // for how big text is on this screen and the menu was well under it. Seven
    // rows that can be read at a glance is the better trade on a display you
    // are wearing rather than looking at.
    const float row_h   = 0.132f * s;
    int visible = (int)((bh - head_h) / row_h);
    if (visible < 3) visible = 3;
    if (visible > kVisibleMax) visible = kVisibleMax;
    const float t_head  = 0.058f * s;
    const float t_row   = 0.090f * s;
    const float t_desc  = 0.060f * s;

    std::vector<MenuItem> items = menu_items(menu_tab);
    const int nitems = (int)items.size();
    int sel_item = menu_index;
    if (sel_item < 0) sel_item = 0;
    if (sel_item > nitems - 1) sel_item = nitems > 0 ? nitems - 1 : 0;

    // MenuItem::type says whether a row is a setting or a reading: Build,
    // Version, Decoder and Display report what the goggle is, and there is
    // nothing to choose. Those get no value column at all - an empty panel says
    // "nothing to set here" where a single entry with a filled mark beside it
    // would claim to be a choice with one option.
    const bool read_only = nitems && items[sel_item].type == 0;
    const bool action    = nitems && items[sel_item].type == 3;

    // What the selected row reads right now - the one thing the value column
    // still works out for itself, because it has to be live. Everything either
    // side of it comes from menu_lad_, filled on the key thread.
    //
    // menu_value_text writes "< X >" because it was built for a one-line
    // widget where the chevrons said which key to press.
    char cur_txt[64];
    if (nitems) menu_value_text(menu_tab, sel_item, cur_txt, sizeof(cur_txt));
    else        cur_txt[0] = '\0';
    // The pending mark stays in the setting column, where it belongs to a row.
    // Here it would hang off the end of the value itself.
    tidy_value(cur_txt, false);

    // ---- one column -------------------------------------------------------
    // entries is a flat array of up to two strings per row: a label and, where
    // the column shows one, the value beside it.
    auto column = [&](int ci, const char* head, int count, int sel, bool lit,
                      const char* (*label)(void*, int), const char* (*value)(void*, int),
                      void* ctx, bool (*disabled)(void*, int) = nullptr,
                      bool (*is_header)(void*, int) = nullptr) {
        const float x = colx[ci], w = colw[ci];
        // Opaque enough to read thin type over a bright sky. The old menu sat
        // at 0.7 and washed out; these are its ground colour, not the video's.
        const float ga = lit ? 0.94f : 0.72f;
        // Black first, then the theme's ground over it. The ground is a colour
        // rather than a mask - on its own, bright video reads straight through
        // a dark blue at any alpha.
        draw_panel(x, by, w, bh, lit ? 0.72f : 0.55f, 0.0f, 0.0f, 0.0f);
        draw_panel(x, by, w, bh, ga, T.ground[0], T.ground[1], T.ground[2]);

        // A hairline between columns, so three panels read as one object.
        if (ci > 0)
            draw_panel(x, by, 0.002f, bh, 0.35f, T.quiet[0], T.quiet[1], T.quiet[2]);

        const float hy = by + bh - head_h + pad;
        const float hq = lit ? 1.0f : 0.55f;
        draw_text(head, x + pad, hy, t_head, false,
                  T.text[0] * hq, T.text[1] * hq, T.text[2] * hq);

        int start = window_start(sel, count, visible);
        for (int k = 0; k < visible && start + k < count; k++) {
            const int idx = start + k;
            const bool on = (idx == sel);
            // Rows run downwards from under the header.
            const float ry = by + bh - head_h - (float)(k + 1) * row_h;

            if (is_header && is_header(ctx, idx)) {
                // A label with no value, no selection bar and nothing to
                // press - just a divider between groups of real rows. Sits
                // low and quiet so it reads as structure, not as a setting
                // that happens to be dim.
                const float ty = ry + row_h * 0.5f - t_row * 0.30f * 0.72f;
                draw_text(label(ctx, idx), x + pad * 1.8f, ty, t_row * 0.72f, false,
                          T.quiet[0], T.quiet[1], T.quiet[2]);
                const float rule_y = ry + row_h * 0.14f;
                draw_panel(x + pad * 1.8f, rule_y, w - pad * 3.6f, 0.0015f, 0.3f,
                           T.quiet[0], T.quiet[1], T.quiet[2]);
                continue;
            }

            if (on) {
                // The selection is a bar in the accent when this column has
                // focus and a dim one when it does not - so a column behind you
                // still says what you chose there.
                draw_panel(x, ry, w, row_h, lit ? 0.42f : 0.20f,
                           T.ground[0] * 1.6f, T.ground[1] * 1.6f, T.ground[2] * 1.6f);
                // The bar sits in from the column edge and stops short of the
                // row's full height. Flush and full-height it reads as a border
                // between rows rather than a mark on one.
                const float bar_h = row_h * 0.58f;
                draw_panel(x + pad * 0.5f, ry + (row_h - bar_h) * 0.5f, 0.006f, bar_h, 1.0f,
                           lit ? T.accent[0] : T.quiet[0],
                           lit ? T.accent[1] : T.quiet[1],
                           lit ? T.accent[2] : T.quiet[2]);
            }

            const bool off = disabled && disabled(ctx, idx);
            // Floors raised: at 0.42 an unfocused column was decoration rather
            // than information, and the whole point of keeping it on screen is
            // that it still says what you chose there.
            float f = on ? (lit ? 1.0f : 0.86f) : (lit ? 0.80f : 0.58f);
            if (off) f *= 0.42f;
            const float ty = ry + row_h * 0.5f - t_row * 0.35f;
            const float* base = off ? T.quiet : T.text;
            float lx = x + pad * 1.8f;

            draw_text(label(ctx, idx), lx, ty, t_row, false,
                      base[0] * f, base[1] * f, base[2] * f);

            if (value) {
                const char* v = value(ctx, idx);
                if (v && v[0]) {
                    // Same reason as the value column: a reading is type, and
                    // type wants the text colour turned down rather than the
                    // blue-grey the rules are drawn in.
                    const float vf = on && lit ? 1.0f : f * 0.78f;
                    const float vr = on && lit ? T.accent[0] : T.text[0];
                    const float vg = on && lit ? T.accent[1] : T.text[1];
                    const float vb = on && lit ? T.accent[2] : T.text[2];
                    draw_text(v, x + w - pad, ty, t_row * 0.86f, true,
                              vr * vf, vg * vf, vb * vf);
                }
            }
        }

        // Say when there is more above or below, so a windowed list does not
        // pretend to be the whole list.
        if (start > 0)
            draw_text("\xE2\x96\xB2", x + w * 0.5f, by + bh - head_h - row_h * 0.28f,
                      t_head * 0.7f, false, T.quiet[0], T.quiet[1], T.quiet[2]);
        if (start + visible < count)
            draw_text("\xE2\x96\xBC", x + w * 0.5f, by + desc_h * 0.15f,
                      t_head * 0.7f, false, T.quiet[0], T.quiet[1], T.quiet[2]);
    };

    // A light edge around the whole assembly, so three panels read as one
    // object sitting on the video rather than three that happen to be adjacent.
    auto outline = [&](float o_r, float o_g, float o_b, float a) {
        const float t = 0.0035f;
        draw_panel(bx,          by,           bw, t,  a, o_r, o_g, o_b);   // bottom
        draw_panel(bx,          by + bh - t,  bw, t,  a, o_r, o_g, o_b);   // top
        draw_panel(bx,          by,           t,  bh, a, o_r, o_g, o_b);   // left
        draw_panel(bx + bw - t, by,           t,  bh, a, o_r, o_g, o_b);   // right
    };

    // ---- the three columns ------------------------------------------------
    struct Ctx {
        std::vector<MenuItem>* items;
        OSD* self;
        char vbuf[64];
        bool linked;
    } ctx{ &items, this, {0}, false };

    // VIDEO is the air unit's own settings and cannot be reached with nothing
    // linked - the menu refuses to open on it. Say so: an unreachable row
    // that merely looks a bit dimmer reads as a rendering fault rather than
    // as a state. AR8030 stays lit: unlinked it holds Bind, which is the one
    // thing there is to do without a link.
    const bool linked = (osd_vars.artosyn.state == 2);
    ctx.linked = linked;
    column(0, "SECTION", kMenuTabs, menu_tab, menu_focus == 0,
           [](void*, int i) { return kTabNames[i]; }, nullptr, &ctx,
           [](void* c, int i) { return !((Ctx*)c)->linked && i < 1; });

    column(1, kTabNames[menu_tab], nitems, sel_item, menu_focus == 1,
           [](void* c, int i) { return ((Ctx*)c)->items->at(i).label; },
           [](void* c, int i) {
               Ctx* x = (Ctx*)c;
               x->self->menu_value_text(x->self->menu_tab, i, x->vbuf, sizeof(x->vbuf));
               tidy_value(x->vbuf, true);   // "*" says this row is not applied yet
               return (const char*)x->vbuf;
           }, &ctx, nullptr,
           [](void* c, int i) { return ((Ctx*)c)->items->at(i).type == 2; });

    if (read_only) {
        // Ground only - no header either. The reading is already in the
        // setting column beside its name; repeating it here would just be
        // louder, and a "VALUE" heading over nothing reads as a column that
        // failed to draw rather than one with nothing to say.
        const float x = colx[2], w = colw[2];
        draw_panel(x, by, w, bh, menu_focus == 2 ? 0.80f : 0.55f, 0.0f, 0.0f, 0.0f);
        draw_panel(x, by, w, bh, menu_focus == 2 ? 0.94f : 0.72f,
                   T.ground[0], T.ground[1], T.ground[2]);
        draw_panel(x, by, 0.002f, bh, 0.35f, T.quiet[0], T.quiet[1], T.quiet[2]);
    } else if (action) {
        // An action has nothing to walk through. The column says what Enter
        // will do, in the place a value would sit, and under it how the
        // action is going - which is the only place that state is shown in
        // the menu, so the row itself can stay the plain verb.
        const float x = colx[2], w = colw[2];
        draw_panel(x, by, w, bh, menu_focus == 2 ? 0.72f : 0.55f, 0.0f, 0.0f, 0.0f);
        draw_panel(x, by, w, bh, menu_focus == 2 ? 0.94f : 0.72f,
                   T.ground[0], T.ground[1], T.ground[2]);
        draw_panel(x, by, 0.002f, bh, 0.35f, T.quiet[0], T.quiet[1], T.quiet[2]);
        const float hq = menu_focus == 2 ? 1.0f : 0.55f;
        draw_text("ACTION", x + pad, by + bh - head_h + pad, t_head, false,
                  T.text[0] * hq, T.text[1] * hq, T.text[2] * hq);

        char hint[32], status[48];
        menu_action_status(menu_tab, sel_item, hint, sizeof(hint), status, sizeof(status));
        const float f = menu_focus == 2 ? 1.0f : 0.78f;
        const float avail_w = w - pad * 2.2f;
        auto fit_size = [&](const char* t, float size) {
            const float tw = text_width(t, size);
            if (tw > avail_w && tw > 0.0f) {
                size *= avail_w / tw;
                const float floor_sz = t_row * 0.62f;
                if (size < floor_sz) size = floor_sz;
            }
            return size;
        };
        // Hung from the top like the first entry of a value ladder, so the
        // eye finds it where a value would be on the row above or below.
        const float t_cur = t_row * 1.3f;
        const float cy    = by + bh - head_h - pad - t_cur * 1.15f;
        const float ht    = fit_size(hint, t_cur);
        draw_text(hint, x + (w - text_width(hint, ht)) * 0.5f, cy, ht, false,
                  T.accent[0] * f, T.accent[1] * f, T.accent[2] * f);
        if (status[0]) {
            const float st = fit_size(status, t_row * 0.95f);
            draw_text(status, x + (w - text_width(status, st)) * 0.5f,
                      cy - t_cur * 1.25f, st, false,
                      T.text[0] * f, T.text[1] * f, T.text[2] * f);
        }
    } else {
        // Every settable row, whatever its values are made of: what it reads
        // now in the middle, and what is either side of it fading out with
        // distance. A set and a range are the same question asked of different
        // material - which way does the next press move this, and to what -
        // and they used to be answered in two unrelated shapes, so two
        // settings a row apart looked like two different kinds of control.
        const float x = colx[2], w = colw[2];
        draw_panel(x, by, w, bh, menu_focus == 2 ? 0.72f : 0.55f, 0.0f, 0.0f, 0.0f);
        draw_panel(x, by, w, bh, menu_focus == 2 ? 0.94f : 0.72f,
                   T.ground[0], T.ground[1], T.ground[2]);
        draw_panel(x, by, 0.002f, bh, 0.35f, T.quiet[0], T.quiet[1], T.quiet[2]);
        const float hq = menu_focus == 2 ? 1.0f : 0.55f;
        draw_text("VALUE", x + pad, by + bh - head_h + pad, t_head, false,
                  T.text[0] * hq, T.text[1] * hq, T.text[2] * hq);

        // Unfocused, not hidden: this column is read while the selection is
        // still one to its left - that is the whole point of showing it - so
        // it dims by a quarter rather than by half.
        const float f = menu_focus == 2 ? 1.0f : 0.78f;
        const int span = kMenuLadderSpan;
        // Not every value is a word. Channel reads "5839 MHz [ENTER: SCAN]"
        // and Calib Distance reads a raw sample and an offset - set at the
        // size a one-word value wants, those ran straight out of the column
        // and across the setting names beside it.
        const float avail_w = w - pad * 2.2f;
        auto fit_size = [&](const char* t, float size) {
            const float tw = text_width(t, size);
            if (tw > avail_w && tw > 0.0f) {
                size *= avail_w / tw;
                const float floor_sz = t_row * 0.62f;
                if (size < floor_sz) size = floor_sz;
            }
            return size;
        };
        // Read, never walk: the list is worked out on the key thread
        // (menu_refresh_options) and only read here.
        const char (*lad)[kMenuOptLen] = menu_lad_;
        const bool have = (menu_lad_tab_ == menu_tab && menu_lad_idx_ == sel_item);
        const int  nlad = have ? menu_lad_n_ : 0;
        const int  cur  = have ? menu_lad_cur_ : 0;

        // The value in force sits large in the accent in the middle of the
        // column, always in the same place, and the rest of the list runs
        // away from it in the row's own order - a set as its values are
        // defined, a range ascending - above and below, fading with distance.
        // Nothing wraps: at the first value the slots above are simply empty,
        // at the last the slots below are, so the column reads as a list with
        // ends rather than a wheel that goes on forever. Only when the list
        // is longer than the panel are the far entries cut, and a small arrow
        // says so.
        const float t_cur    = t_row * 1.6f;
        const float t_nb     = t_row * 0.88f;
        const float band_top = by + bh - head_h - pad;
        const float band_bot = by + bh * 0.28f;          // clear of the help text
        const float step     = t_cur;
        const float cy       = band_bot + (band_top - band_bot - t_cur) * 0.5f;
        // How many entries each side the panel can hold around the centre.
        int fit = (int)((band_top - band_bot - t_cur) * 0.5f / step);
        if (fit < 1) fit = 1;
        const int above = cur < fit ? cur : fit;
        const int below = (nlad - 1 - cur) < fit ? (nlad - 1 - cur) : fit;

        for (int k = cur - above; k <= cur + below; k++) {
            if (k < 0 || k >= nlad) continue;
            const bool on = (k == cur);
            // The current entry is drawn from the live reading, not from the
            // copy taken at the last key: Channel and the like move on their own.
            const char* txt = on ? cur_txt : lad[k];
            const int   ad  = k < cur ? cur - k : k - cur;
            const float ts  = fit_size(txt, on ? t_cur : t_nb);
            const float tw  = text_width(txt, ts);
            // Earlier in the list is higher on screen.
            const float y   = cy + (float)(cur - k) * step + (on ? 0.0f : (t_cur - t_nb) * 0.5f);
            if (on) {
                draw_text(txt, x + (w - tw) * 0.5f, y, ts, false,
                          T.accent[0] * f, T.accent[1] * f, T.accent[2] * f);
            } else {
                float a = 0.90f - 0.13f * (float)ad;
                if (a < 0.50f) a = 0.50f;
                draw_text(txt, x + (w - tw) * 0.5f, y, ts, false,
                          T.text[0] * a * f, T.text[1] * a * f, T.text[2] * a * f);
            }
        }
        if (nlad == 0) {
            // Nothing enumerated (a row the walk cannot step): the reading alone.
            const float ts = fit_size(cur_txt, t_cur);
            draw_text(cur_txt, x + (w - text_width(cur_txt, ts)) * 0.5f, cy, ts, false,
                      T.accent[0] * f, T.accent[1] * f, T.accent[2] * f);
        }
        if (cur - above > 0)
            draw_text("\xE2\x96\xB2", x + w * 0.5f, cy + (float)above * step + t_cur * 1.05f,
                      t_head * 0.7f, false, T.quiet[0], T.quiet[1], T.quiet[2]);
        if (cur + below < nlad - 1)
            draw_text("\xE2\x96\xBC", x + w * 0.5f, cy - (float)below * step - t_head * 0.9f,
                      t_head * 0.7f, false, T.quiet[0], T.quiet[1], T.quiet[2]);
    }

    outline(T.quiet[0], T.quiet[1], T.quiet[2], 0.85f);

    // ---- the line that says what the setting does -------------------------
    // The help line lives at the foot of the value column. A setting rarely has
    // more than a handful of values, so that column is mostly empty below them
    // - and a strip across the whole box cost every column a row of height to
    // say something about only one of them.
    if (nitems) {
        const char* d = menu_help_text(menu_tab, sel_item, items[sel_item].help);
        // What the arrows changed and Enter has not sent, as "from -> to".
        // Sits just above the help line, in the accent, because it is the
        // one thing on this panel that is about to act on the aircraft.
        char pending[80] = {0};
        if (menu_tab >= 0 && menu_tab < kMenuTabs && sel_item < kMenuRows &&
            menu_dirty[menu_tab][sel_item] && menu_pending_from[menu_tab][sel_item][0]) {
            char to[32];
            menu_plain_value(menu_tab, sel_item, to, sizeof(to));
            snprintf(pending, sizeof(pending), "Pending: %s -> %s",
                     menu_pending_from[menu_tab][sel_item], to);
        }
        // A row can also carry a warning, drawn in amber below its help and
        // shown whatever the row's value - something to read before changing
        // it, not a description of it.
        const char* warn = menu_help_warning(menu_tab, sel_item);
        if ((d && d[0]) || pending[0] || (warn && warn[0])) {
            const float cx = colx[2], cwv = colw[2];
            const float avail = cwv - pad * 3.0f;

            // Greedy wrap on spaces, measured in the font it will be drawn in.
            // Lines stack upward from the foot of the value column, which is
            // mostly empty; ten covers a warning plus the canopy's Betaflight
            // setup. A '\n' in the text starts a new line.
            enum { kHelpLines = 10 };
            char line[kHelpLines][96];
            bool amber[kHelpLines] = {};
            int nlines = 0;
            auto wrap = [&](const char* d, bool is_warn) {
                char cur[96]; cur[0] = '\0';
                const char* p = d;
                const int first = nlines;
                while (*p && nlines < kHelpLines) {
                    const char* sp = strpbrk(p, " \n");
                    size_t wlen = sp ? (size_t)(sp - p) : strlen(p);
                    char word[64];
                    if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
                    memcpy(word, p, wlen); word[wlen] = '\0';

                    char trial[96];
                    snprintf(trial, sizeof(trial), "%s%s%s", cur, cur[0] ? " " : "", word);
                    if (cur[0] && text_width(trial, t_desc) > avail) {
                        snprintf(line[nlines++], sizeof(line[0]), "%s", cur);
                        snprintf(cur, sizeof(cur), "%s", word);
                    } else {
                        snprintf(cur, sizeof(cur), "%s", trial);
                    }
                    if (sp && *sp == '\n' && cur[0] && nlines < kHelpLines) {
                        snprintf(line[nlines++], sizeof(line[0]), "%s", cur);
                        cur[0] = '\0';
                    }
                    p = sp ? sp + 1 : p + strlen(p);
                }
                if (cur[0] && nlines < kHelpLines) snprintf(line[nlines++], sizeof(line[0]), "%s", cur);
                for (int k = first; k < nlines; k++) amber[k] = is_warn;
            };
            if (d && d[0]) wrap(d, false);           // first = top
            if (warn && warn[0]) wrap(warn, true);   // under the description

            const float lh   = t_desc * 1.35f;
            const float base = by + pad * 1.2f;
            const float rule = base + lh * (float)nlines + pad * 0.8f;
            draw_panel(cx + pad, rule, cwv - pad * 2.0f, 0.003f, 0.45f,
                       T.quiet[0], T.quiet[1], T.quiet[2]);
            for (int k = 0; k < nlines; k++) {
                const int li = nlines - 1 - k;
                if (amber[li])
                    draw_text(line[li], cx + pad * 1.8f, base + lh * (float)k, t_desc, false,
                              1.0f, 0.75f, 0.0f);
                else
                    draw_text(line[li], cx + pad * 1.8f, base + lh * (float)k, t_desc, false,
                              T.text[0] * 0.70f, T.text[1] * 0.70f, T.text[2] * 0.70f);
            }
            if (pending[0])
                draw_text(pending, cx + pad * 1.8f, rule + pad * 1.2f, t_desc, false,
                          T.accent[0], T.accent[1], T.accent[2]);
        }
    }
}

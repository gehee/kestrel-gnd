// The settings, as a blade of the canopy unfolded up its side of the screen.
//
// The left blade holds the goggle's settings (HUD, DISPLAY, DVR, SYSTEM), the
// right one the air unit's (VIDEO, RADIO) - each the settings for what that
// blade shows. Unfolded, a blade is a column at its edge of the picture, so
// the middle stays clear and a change to the picture or the HUD is seen as it
// is made. Its sections are tabs along the top (Left/Right), its rows a list
// (Up/Down); OK on a row starts changing its value in place - the row opens to
// show the values either side - and OK again applies it.
//
// Drawn the gallery's way, in screen pixels from the top left, y down
// (osd_screen.cpp): one flat ground, a ring on what is selected.

#include "osd.hpp"
#include "hud_theme.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kVisibleMax = 16;

// Tab names: AR8030 is the radio, and on the air unit's blade it says so.
constexpr const char* kTabNames[8] = { "VIDEO", "RADIO", "HUD", "DISPLAY", "DVR", "SYSTEM", "INFO", "SERIAL" };

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

// "< 5px > *" reads as "5px *", or as "5px" where the mark does not belong:
// the chevrons were for a widget that is gone, and the pending mark is lifted
// off first or the chevron test cannot see the closing one under it.
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

// side: 0 the goggle's blade, 1 the air unit's. x, y, w, h: the unfolded
// blade, in pixels. u: pixels per 1080p pixel, with the UI scale in it.
void OSD::draw_side_panel(int side, float x, float y, float w, float h, float u) {
    const HudTheme& T = hud_theme_current();
    const float amber[3] = { 0.95f, 0.79f, 0.30f };
    const float lit_bg[3] = { std::fmin(1.0f, T.ground[0] * 1.6f), std::fmin(1.0f, T.ground[1] * 1.6f),
                              std::fmin(1.0f, T.ground[2] * 1.6f) };
    float tmp[3];
    auto scaled = [&](const float* c, float f) { tmp[0] = c[0] * f; tmp[1] = c[1] * f; tmp[2] = c[2] * f; return (const float*)tmp; };

    const float pad = 22 * u;
    const float f_row = 23 * u, f_val = 20 * u, f_hdr = 15 * u, f_help = 18 * u, f_tab = 21 * u;
    const float row_h = 50 * u;
    const float ring = 3 * u;

    // ---- whose settings, and its sections ----
    px_text(side == 0 ? "GOGGLE" : "AIR UNIT", x + pad, y + pad + 14 * u, 15 * u, 0, T.accent, 1.0f);
    const std::vector<int> tabs = ui_side_tabs(side);
    float tx = x + pad;
    const float tab_base = y + pad + 52 * u;
    for (int t : tabs) {
        const bool on = (t == menu_tab);
        const float tw = px_text(kTabNames[t], tx, tab_base, f_tab, 0, on ? T.text : T.quiet, on ? 1.0f : 0.8f);
        if (on) px_fill(tx, tab_base + 9 * u, tw, 3 * u, T.accent, 1.0f);
        tx += tw + 26 * u;
    }
    const float list_top = tab_base + 26 * u;
    px_fill(x + pad, list_top - 6 * u, w - 2 * pad, 1.2f * u, T.quiet, 0.3f);

    std::vector<MenuItem> items = menu_items(menu_tab);
    const int n = (int)items.size();
    int sel = menu_index;
    if (sel < 0) sel = 0;
    if (sel > n - 1) sel = n > 0 ? n - 1 : 0;
    const bool editing = (menu_focus == 2);

    // ---- what the selected row does, at the foot (worked out first: the list stops above it) ----
    enum { kHelpLines = 10 };
    char line[kHelpLines][96];
    const float* line_col[kHelpLines];
    int nlines = 0;
    const float avail = w - 2 * pad;
    auto wrap = [&](const char* text, const float* col) {
        char cur[96]; cur[0] = '\0';
        const char* p = text;
        while (*p && nlines < kHelpLines) {
            const char* sp = strpbrk(p, " \n");
            size_t wlen = sp ? (size_t)(sp - p) : strlen(p);
            char word[64];
            if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
            memcpy(word, p, wlen); word[wlen] = '\0';
            char trial[96];
            snprintf(trial, sizeof(trial), "%s%s%s", cur, cur[0] ? " " : "", word);
            if (cur[0] && px_text_w(trial, f_help) > avail) {
                line_col[nlines] = col;
                snprintf(line[nlines++], sizeof(line[0]), "%s", cur);
                snprintf(cur, sizeof(cur), "%s", word);
            } else snprintf(cur, sizeof(cur), "%s", trial);
            if (sp && *sp == '\n' && cur[0] && nlines < kHelpLines) {
                line_col[nlines] = col;
                snprintf(line[nlines++], sizeof(line[0]), "%s", cur);
                cur[0] = '\0';
            }
            p = sp ? sp + 1 : p + strlen(p);
        }
        if (cur[0] && nlines < kHelpLines) { line_col[nlines] = col; snprintf(line[nlines++], sizeof(line[0]), "%s", cur); }
    };
    const float help_col[3] = { T.text[0] * 0.70f, T.text[1] * 0.70f, T.text[2] * 0.70f };
    if (n) {
        // What the arrows changed and OK has not sent, as "from -> to".
        char pending[80] = {0};
        if (menu_tab >= 0 && menu_tab < kMenuTabs && sel < kMenuRows &&
            menu_dirty[menu_tab][sel] && menu_pending_from[menu_tab][sel][0]) {
            char to[32];
            menu_plain_value(menu_tab, sel, to, sizeof(to));
            snprintf(pending, sizeof(pending), "Pending: %s -> %s. OK sends it.", menu_pending_from[menu_tab][sel], to);
        }
        if (pending[0]) wrap(pending, T.accent);
        if (items[sel].type == 3) {                    // how an action is going
            char hint[32], status[48];
            menu_action_status(menu_tab, sel, hint, sizeof(hint), status, sizeof(status));
            if (status[0]) wrap(status, T.accent);
        }
        const char* d = menu_help_text(menu_tab, sel, items[sel].help);
        if (d && d[0]) wrap(d, help_col);
        const char* warn = menu_help_warning(menu_tab, sel);
        if (warn && warn[0]) wrap(warn, amber);
    }
    const float lh = f_help * 1.4f;
    const float help_top = y + h - pad - lh * (float)nlines;
    if (nlines) {
        px_fill(x + pad, help_top - 10 * u, w - 2 * pad, 1.2f * u, T.quiet, 0.35f);
        for (int k = 0; k < nlines; k++)
            px_text(line[k], x + pad, help_top + lh * (float)k + f_help * 0.9f, f_help, 0, line_col[k], 1.0f);
    }

    // ---- the rows ----
    const float list_bottom = help_top - 20 * u;
    const float open_extra = editing ? row_h * 1.6f : 0.0f;    // the row being changed opens up
    int visible = (int)((list_bottom - list_top - open_extra) / row_h);
    if (visible < 3) visible = 3;
    if (visible > kVisibleMax) visible = kVisibleMax;
    const int start = window_start(sel, n, visible);

    char cur_txt[64];
    if (n) menu_value_text(menu_tab, sel, cur_txt, sizeof(cur_txt));
    else cur_txt[0] = '\0';
    tidy_value(cur_txt, false);

    float ry = list_top;
    for (int k = 0; k < visible && start + k < n; k++) {
        const int idx = start + k;
        const MenuItem& it = items[idx];
        if (it.type == 2) {                            // a group's name, not a row
            px_text(it.label, x + pad, ry + row_h * 0.72f, f_hdr, 0, T.quiet, 0.9f);
            px_fill(x + pad, ry + row_h * 0.86f, w - 2 * pad, 1.2f * u, T.quiet, 0.3f);
            ry += row_h;
            continue;
        }
        const bool on = (idx == sel);
        if (it.type == 4) {                            // a list, wrapped where ", " allows
            const float lh = f_row * 1.4f, room = w - 2 * pad;
            std::vector<std::string> lines(1);
            const std::string text = it.label;
            for (size_t p = 0; p < text.size();) {
                size_t e = text.find(", ", p);
                const std::string part = text.substr(p, e == std::string::npos ? std::string::npos : e + 1 - p);
                p = e == std::string::npos ? text.size() : e + 2;
                const std::string trial = lines.back().empty() ? part : lines.back() + " " + part;
                if (!lines.back().empty() && px_text_w(trial.c_str(), f_row) > room) lines.push_back(part);
                else lines.back() = trial;
            }
            const float rh = std::fmax(row_h, lh * (float)lines.size() + (row_h - lh));
            if (ry + rh > list_bottom + 6 * u) break;  // no room for all of it: stop above the help
            if (on) {
                px_fill(x + pad * 0.5f, ry + 2 * u, w - pad, rh - 4 * u, lit_bg, 0.95f);
                px_frame(x + pad * 0.5f, ry + 2 * u, w - pad, rh - 4 * u, ring, T.accent, 1.0f);
            }
            for (size_t l = 0; l < lines.size(); l++)
                px_text(lines[l].c_str(), x + pad, ry + (row_h - lh) * 0.5f + lh * (float)l + lh * 0.5f + f_row * 0.36f,
                        f_row, 0, scaled(T.text, on ? 1.0f : 0.82f), 1.0f);
            ry += rh;
            continue;
        }
        const bool open_row = on && editing;
        const float rh = open_row ? row_h + open_extra : row_h;
        if (on) {
            px_fill(x + pad * 0.5f, ry + 2 * u, w - pad, rh - 4 * u, lit_bg, 0.95f);
            px_frame(x + pad * 0.5f, ry + 2 * u, w - pad, rh - 4 * u, ring, T.accent, 1.0f);
        }
        const float base = ry + row_h * 0.5f + f_row * 0.36f;
        px_text(it.label, x + pad, base, f_row, 0, scaled(T.text, on ? 1.0f : 0.82f), 1.0f);

        if (open_row) {
            // The values either side of the one in force, as in the old value
            // column: earlier above, later below, fading with distance.
            const bool have = (menu_lad_tab_ == menu_tab && menu_lad_idx_ == sel);
            const int nlad = have ? menu_lad_n_ : 0, c = have ? menu_lad_cur_ : 0;
            const float cy = ry + rh * 0.5f, vx = x + w - pad;
            const float f_cur = 30 * u, f_nb = 18 * u, step = 34 * u;
            for (int d = -1; d <= 1; d++) {
                const int k2 = c + d;
                if (d != 0 && (k2 < 0 || k2 >= nlad)) continue;
                const char* txt = d == 0 ? cur_txt : menu_lad_[k2];
                const float fs = d == 0 ? f_cur : f_nb;
                px_text(txt, vx, cy + d * step + fs * 0.36f, fs, 2, d == 0 ? T.accent : T.quiet, d == 0 ? 1.0f : 0.8f);
            }
        } else {
            char v[64];
            if (on && it.type == 3) {
                char hint[32], status[48];
                menu_action_status(menu_tab, idx, hint, sizeof(hint), status, sizeof(status));
                snprintf(v, sizeof(v), "%s", hint);
            } else {
                menu_value_text(menu_tab, idx, v, sizeof(v));
                tidy_value(v, true);                   // "*": changed, not sent yet
            }
            if (v[0]) {
                float vs = f_val;
                const float room = w - 2 * pad - px_text_w(it.label, f_row) - 16 * u;
                const float vw = px_text_w(v, vs);
                if (vw > room && room > 0) vs = std::fmax(vs * room / vw, f_val * 0.6f);
                const float* vc = on ? T.accent : (it.type == 0 ? T.quiet : T.text);
                px_text(v, x + w - pad, base, vs, 2, vc, on ? 1.0f : 0.85f);
            }
        }
        ry += rh;
    }
    // Say when there is more above or below.
    auto arrow = [&](float cx, float cy, bool up) {
        const float s = 6 * u;
        const float p[8] = { cx - s, up ? cy + s * 0.5f : cy - s * 0.5f,  cx + s, up ? cy + s * 0.5f : cy - s * 0.5f,
                             cx, up ? cy - s * 0.5f : cy + s * 0.5f,  cx, up ? cy - s * 0.5f : cy + s * 0.5f };
        px_quad(p, T.quiet, 0.8f);
    };
    if (start > 0) arrow(x + w * 0.5f, list_top + 2 * u, true);
    if (start + visible < n) arrow(x + w * 0.5f, list_bottom + 6 * u, false);
}

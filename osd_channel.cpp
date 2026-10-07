// The channel band: what is on every channel, where the link is, and what is
// allowed where you fly. RADIO > Channel unfolds the canopy's right blade along
// the bottom of the picture into it (draw_canopy_ui), so the picture stays in
// view, full size, while the link moves.
//
// One cursor walks AUTO and the radio's channels, sorted by frequency. Bars are
// the radio's averaged scan energy now; a tick above a bar is the 30-second
// peak, so a channel that only looks quiet gives itself away. A rail along the
// top of the chart says what the country of SYSTEM > Time Zone allows
// (zones.hpp): OK with its power cap where it is licence-exempt (certified
// equipment), OK - AMATEUR or LICENCE NEEDED in its ITU region's amateur band,
// INDOOR ONLY, NOT ALLOWED. It is an indicator only: any channel can still be
// picked. Busy-ness keeps the bar colours (the theme's data hue, the gallery's
// amber and danger); legality never uses them on the bars, only hatching, grey
// and outlined pills, so no colour means two things.
//
// Keys: left/right walk (and stop at the ends), up jumps to the quietest
// channel the pilot may use, by its peak, down back to the link's. OK moves
// the link there at once, and on AUTO hands the channel back to the radio,
// which is what the old Channel Hop row did.
//
// Cheap to draw and smooth: its shapes go to the GPU in one batch
// (osd_screen.cpp), each channel is judged once a frame, and the printed
// numbers are taken twice a second, so the scan's 14 updates a second do not
// each rasterise new text. The cursor, the LINK marker and the bars glide to
// where they belong.
//
// Laid out in hundredths of the band's width, from its top left.

#include "osd.hpp"
#include "artosyn/ar8030_source.hpp"
#include "hud_theme.hpp"
#include "zones.hpp"
#include "utils/time_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const float kAmber[3]  = { 0.95f, 0.79f, 0.30f };
const float kDanger[3] = { 0.93f, 0.33f, 0.28f };

// Busy-ness from the radio's averaged scan energy: free, some traffic, busy.
int busy_level(float dbm) { return dbm <= -88.0f ? 0 : (dbm <= -80.0f ? 1 : 2); }

const char* kBusyName[3] = { "QUIET", "SOME TRAFFIC", "BUSY" };

void cap_text(int mw, char* out, size_t n) {
    if (mw >= 1000 && mw % 1000 == 0) snprintf(out, n, "%d W", mw / 1000);
    else snprintf(out, n, "%d mW", mw);
}

// Close the gap to the target by 1 - e^(-dt/tau); true while still moving.
bool glide(float& v, float target, float dt, float tau) {
    v += (target - v) * (1.0f - std::exp(-dt / tau));
    if (std::fabs(target - v) < 0.004f) v = target;
    return v != target;
}

}  // namespace

// The link's own width, which is what a channel is judged by.
int OSD::chan_bw_mhz() const {
    const int idx = osd_vars.artosyn.rf_bw_idx;
    const int bw = (int)std::lround(atof(ar_bw_label(idx)));
    return bw > 0 ? bw : 20;
}

// The power the air unit sends at, in mW; an AUTO level counts as its cap.
int OSD::chan_tx_mw() const {
    const ar_pwr_level& l = kArPwrLevels[ar_pwr_index(Ar8030Source::tx_power_mw)];
    return l.automode ? l.mw - 1 : l.mw;
}

// The country whose rules apply (-1: no time zone set), and whether any are known for it.
static int rules_country(bool& known) {
    const int cc = zones::selected_country();
    known = zones::has_rules(cc);
    return cc;
}

// The channel the cursor's Up goes to: the lowest 30-second peak among the
// channels the pilot may use (all of them while there are no rules).
int OSD::chan_quietest() const {
    const chan_scan_info& sc = osd_vars.chan_scan;
    const int bw = chan_bw_mhz();
    const bool lic = zones::licensed();
    bool known;
    const int cc = rules_country(known);
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int r = 0; r < chan_order_n_; r++) {
            const int ci = chan_order_[r];
            if (pass == 0 && known &&
                zones::judge(cc, sc.freq_mhz[ci], bw, lic).state != zones::kAllowed) continue;
            if (best < 0 || scan_peak_dbm[ci] < scan_peak_dbm[chan_order_[best]]) best = r;
        }
    return best;
}

void OSD::set_chan_scan(const chan_scan_info& sc) {
    pthread_mutex_lock(&osd_mutex);
    osd_vars.chan_scan = sc;
    const int n = std::min(std::max(sc.chan_num, 0), 64);
    // 30 s max-hold per channel: raised at once when a sample is as busy or
    // busier, held for 30 s, then let down to the current level.
    const uint64_t now = get_time_us();
    for (int i = 0; i < n; i++) {
        if (scan_peak_ts[i] == 0 || sc.power_dbm[i] >= scan_peak_dbm[i] ||
            (now - scan_peak_ts[i]) > 30ULL * 1000000ULL) {
            scan_peak_dbm[i] = sc.power_dbm[i];
            scan_peak_ts[i]  = now;
        }
    }
    // Sorted once here, for the drawing and the keys alike.
    for (int i = 0; i < n; i++) chan_order_[i] = i;
    std::sort(chan_order_, chan_order_ + n, [&](int a, int b) { return sc.freq_mhz[a] < sc.freq_mhz[b]; });
    chan_order_n_ = n;
    if (menu_scan_sel > n) menu_scan_sel = n;
    if (menu_scan_open) { render_requested = true; pthread_cond_signal(&osd_cond); }
    pthread_mutex_unlock(&osd_mutex);
}

// Open the page, on the link's channel.
void OSD::chan_open() {
    menu_scan_open = true;
    menu_scan_sel = 0;
    const int wc = osd_vars.chan_scan.work_chan;
    for (int r = 0; r < chan_order_n_; r++)
        if (chan_order_[r] == wc) menu_scan_sel = r + 1;
    // Everything is placed at once on the first frame, not glided in from wherever it was.
    chan_cursor_x_ = chan_link_x_ = -1.0f;
    chan_bars_set_ = false;
    chan_shown_us_ = 0;
    chan_anim_us_ = 0;
    if (cmd_cb) cmd_cb(0x200, 1);   // poll the scan fast while it is open
}

void OSD::chan_close() {
    menu_scan_open = false;
    if (cmd_cb) cmd_cb(0x200, 0);   // back to slow background polling
}

// A key while the page is open. Called under osd_mutex.
void OSD::chan_key(int key) {
    const bool up    = (key == 'w' || key == 'W' || key == KEY_UP    || key == 0x101);
    const bool down  = (key == 's' || key == 'S' || key == KEY_DOWN  || key == 0x102);
    const bool left  = (key == 'a' || key == 'A' || key == KEY_LEFT  || key == 0x104);
    const bool right = (key == 'd' || key == 'D' || key == KEY_RIGHT || key == 0x103);
    const bool enter = (key == 13 || key == '\n');
    const bool back  = (key == kKeyBack || key == 27 || key == 'm' || key == 'M');
    const chan_scan_info& sc = osd_vars.chan_scan;
    const int n = chan_order_n_;

    if (back) { chan_close(); return; }
    if (left)  { if (menu_scan_sel > 0) menu_scan_sel--; return; }
    if (right) { if (menu_scan_sel < n) menu_scan_sel++; return; }
    if (up)    { const int q = chan_quietest(); if (q >= 0) menu_scan_sel = q + 1; return; }
    if (down) {
        for (int r = 0; r < n; r++) if (chan_order_[r] == sc.work_chan) menu_scan_sel = r + 1;
        return;
    }
    if (!enter) return;
    // OK acts at once, no question.
    if (menu_scan_sel == 0) {
        // Back to AUTO: the radio searches and hops by itself again.
        if (!sc.auto_mode) {
            printf("channel page: back to AUTO\n");
            menu_ar_hop = true;
            if (cmd_cb) cmd_cb(0x316, 1);
        }
        return;
    }
    if (menu_scan_sel > n) return;
    // Whatever the rules say: they are shown, not enforced.
    const int ci = chan_order_[menu_scan_sel - 1];
    if (ci == sc.work_chan && !sc.auto_mode) return;   // already fixed there
    // Both ends move: the air unit is told over the sky link and the ground
    // retunes to match (Ar8030Source::set_rf_channel). A channel picked here
    // is manual mode for this session; the next start searches again.
    const int mhz = sc.freq_mhz[ci];
    if (mhz > 0) {
        printf("channel page: moving the link to %d MHz\n", mhz);
        Ar8030Source::request_rf(Ar8030Source::RF_CHAN, mhz * 1000);
        menu_ar_hop = false;
    }
}

bool OSD::draw_channel_band(int W, int H, float band_x, float band_y, float band_w, float band_hp, float t) {
    if (t <= 0.003f) return false;
    const HudTheme& T = hud_theme_current();
    // What it shows, taken under the lock: the scan arrives on the radio's thread.
    pthread_mutex_lock(&osd_mutex);
    const chan_scan_info sc = osd_vars.chan_scan;
    const artosyn_stats art = osd_vars.artosyn;
    int order[64], peak[64];
    const int n = chan_order_n_;
    memcpy(order, chan_order_, sizeof(order));
    memcpy(peak, scan_peak_dbm, sizeof(peak));
    int cur = menu_scan_sel;
    const int q = chan_quietest();
    const int bw = chan_bw_mhz();
    bool rules;
    const int cc = rules_country(rules);
    pthread_mutex_unlock(&osd_mutex);
    const bool lic = zones::licensed();
    const char* cname = cc >= 0 ? zones::country(cc).name : "";
    if (cur > n) cur = n;

    // ---- motion ----
    const uint64_t now_us = get_time_us();
    float dt = chan_anim_us_ ? (float)(now_us - chan_anim_us_) / 1e6f : 1.0f / 60.0f;
    chan_anim_us_ = now_us;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;   // a stall is not a leap
    int link_r = -1;
    for (int r = 0; r < n; r++) if (order[r] == sc.work_chan) link_r = r;
    bool moving = false;
    if (chan_cursor_x_ < 0.0f) chan_cursor_x_ = (float)cur;
    moving |= glide(chan_cursor_x_, (float)cur, dt, 0.055f);
    if (link_r >= 0) {
        if (chan_link_x_ < 0.0f) chan_link_x_ = (float)(link_r + 1);
        moving |= glide(chan_link_x_, (float)(link_r + 1), dt, 0.12f);
    }
    for (int r = 0; r < n; r++) {
        const int ci = order[r];
        if (!chan_bars_set_) { chan_bar_[ci] = (float)sc.power_dbm[ci]; chan_peak_[ci] = (float)peak[ci]; continue; }
        moving |= glide(chan_bar_[ci], (float)sc.power_dbm[ci], dt, 0.07f);
        moving |= glide(chan_peak_[ci], (float)peak[ci], dt, 0.07f);
    }
    chan_bars_set_ = n > 0;
    // The printed numbers, twice a second.
    if (!chan_shown_us_ || now_us - chan_shown_us_ >= 500000) {
        chan_shown_us_ = now_us;
        for (int i = 0; i < 64; i++) { chan_shown_now_[i] = sc.power_dbm[i]; chan_shown_peak_[i] = peak[i]; }
        chan_shown_linked_ = art.state == 2;
        chan_shown_snr_ = (int)std::lround(art.snr);
        chan_shown_mcs_ = ar_mcs_label(art.mcs);
    }

    // Each channel judged once.
    zones::Verdict vd[64];
    for (int r = 0; r < n; r++) vd[r] = zones::judge(cc, sc.freq_mhz[order[r]], bw, lic);

    // Laid out in units of a hundredth of the band's width, from its top left.
    px_begin(W, H, band_x + band_w * 0.5f, band_y + band_hp * 0.5f, 1.0f, t);
    const float c = band_w / 100.0f;
    auto X = [&](float v) { return band_x + v * c; };
    auto Y = [&](float v) { return band_y + v * c; };
    // Text set by its top: the baseline sits 0.87 of the size below.
    auto txt = [&](const char* s, float x, float top, float size, const float* col, int align = 0, float a = 1.0f) {
        return px_text(s, X(x), Y(top + size * 0.87f), size * c, align, col, a) / c;
    };
    const float* busy_col[3] = { T.data, kAmber, kDanger };
    char b[200];
    const int tx = chan_tx_mw();
    const int link_mhz = link_r >= 0 ? sc.freq_mhz[sc.work_chan] : art.tx_freq;
    const float band_h = band_hp / c;     // the band's height, in its own units

    // ---- what the cursor is on, and where the link is ----
    px_fill(X(2.0f), Y(4.2f), 96.0f * c, 0.12f * c, T.text, 0.85f);
    const char* source = "";
    if (cur == 0) {
        snprintf(b, sizeof(b), "AUTO   -   %s", sc.auto_mode ? "the link picks a channel and hops by itself"
                                                           : "OK lets the link pick and hop by itself again");
        txt(b, 2.2f, 1.3f, 1.5f, T.text);
    } else if (n > 0) {
        const int ci = order[cur - 1];
        const zones::Verdict& v = vd[cur - 1];
        source = v.note;
        float x = 2.2f;
        snprintf(b, sizeof(b), "%d MHz", sc.freq_mhz[ci]);
        x += txt(b, x, 1.3f, 1.5f, T.text) + 1.4f;
        const int lv = busy_level((float)chan_shown_peak_[ci]);
        snprintf(b, sizeof(b), "%s   %d dBm, 30 s peak %d", kBusyName[lv], chan_shown_now_[ci], chan_shown_peak_[ci]);
        x += txt(b, x, 1.55f, 1.15f, busy_col[lv]) + 1.4f;
        const float* lc = !rules ? T.quiet : v.state == zones::kNotAllowed ? kDanger : v.state == zones::kIndoor ? kAmber :
                          v.state == zones::kLicence ? T.data : T.text;
        if (!rules) snprintf(b, sizeof(b), cc < 0 ? "time zone not set" : "no rules known for %s", cname);
        else if (v.state == zones::kNotAllowed) snprintf(b, sizeof(b), "not allowed in %s", cname);
        else if (v.state == zones::kLicence) snprintf(b, sizeof(b), "needs an amateur licence in %s", cname);
        else if (v.state == zones::kIndoor) snprintf(b, sizeof(b), "indoor only in %s", cname);
        else if (v.amateur) snprintf(b, sizeof(b), "allowed with your amateur licence");
        else if (v.cap_mw > 0) {
            char cb[16]; cap_text(v.cap_mw, cb, sizeof(cb));
            snprintf(b, sizeof(b), tx > v.cap_mw ? "allowed, %s max (TX Power is %d mW)" : "allowed, %s max", cb, tx);
        } else snprintf(b, sizeof(b), "allowed");
        txt(b, x, 1.55f, 1.15f, lc);
    }
    snprintf(b, sizeof(b), "LINK %d MHz  %s   %s", link_mhz, sc.auto_mode ? "AUTO" : "FIXED",
             cc >= 0 ? zones::country(cc).code : "");
    txt(b, 97.8f, 1.55f, 1.1f, T.quiet, 2);

    // ---- the chart ----
    const float x0 = 2.2f, x1 = 97.8f;
    const float yT = 6.0f + 2.6f, yB = band_h - 4.6f;
    auto Yd = [&](float dbm) { float k = (dbm + 100.0f) / 40.0f; k = std::max(0.0f, std::min(1.0f, k)); return yB - k * (yB - yT); };
    if (n <= 0) txt("SCANNING", (x0 + x1) * 0.5f, (yT + yB) * 0.5f - 1.0f, 2.0f, T.quiet, 1);

    // Slots: AUTO, then each band of channels, a gap between bands (a jump of
    // more than 100 MHz starts a new one).
    int band_of[64], nb = 0, band_lo[8], band_hi[8];
    for (int r = 0; r < n; r++) {
        if (r == 0 || (sc.freq_mhz[order[r]] - sc.freq_mhz[order[r - 1]] > 100 && nb < 8)) {
            band_lo[nb] = r; nb++;
        }
        band_of[r] = nb - 1;
        band_hi[nb - 1] = r;
    }
    const float gap = 1.4f;
    const float units = 1.0f + (float)n + gap * (float)std::max(nb, 1);
    const float sw = (x1 - x0) / units, bwid = sw * 0.62f;
    auto slot_x = [&](int pos) {   // pos 0: AUTO, r + 1: the r-th channel
        if (pos <= 0 || n <= 0) return x0 + 0.5f * sw;
        const int r = std::min(pos, n) - 1;
        return x0 + (1.0f + gap * (float)(band_of[r] + 1) + (float)r + 0.5f) * sw;
    };
    // Between slots, for what glides: across a band's gap too.
    auto slot_xf = [&](float pos) {
        const int i = (int)std::floor(pos);
        const float f = pos - (float)i;
        if (f <= 0.0f || i >= n) return slot_x(i);
        return slot_x(i) + (slot_x(i + 1) - slot_x(i)) * f;
    };

    // The rule rail along the top of the chart: runs of channels that share a verdict.
    const float ry = 6.0f, rh = 1.7f;
    if (n > 0 && !rules) {
        const float xa = slot_x(1) - sw * 0.5f, xb = slot_x(n) + sw * 0.5f;
        px_frame(X(xa), Y(ry), (xb - xa) * c, rh * c, 0.1f * c, T.quiet, 0.6f);
        if (cc < 0) snprintf(b, sizeof(b), "TIME ZONE NOT SET  -  SYSTEM > TIME ZONE");
        else snprintf(b, sizeof(b), "NO RULES KNOWN FOR %s", cname);
        txt(b, (xa + xb) * 0.5f, ry + 0.4f, 0.85f, kAmber, 1);
    } else if (n > 0) {
        int r = 0;
        while (r < n) {
            const zones::Verdict& v = vd[r];
            int e = r;
            while (e + 1 < n && band_of[e + 1] == band_of[r] && vd[e + 1].state == v.state &&
                   vd[e + 1].cap_mw == v.cap_mw && vd[e + 1].amateur == v.amateur) e++;
            const float xa = slot_x(r + 1) - sw * 0.5f + 0.08f, xb = slot_x(e + 1) + sw * 0.5f - 0.08f;
            const float* col = v.state == zones::kNotAllowed ? kDanger : v.state == zones::kIndoor ? kAmber :
                               v.state == zones::kLicence ? T.data : T.quiet;
            if (v.state == zones::kAllowed)
                px_fill(X(xa), Y(ry), (xb - xa) * c, 0.14f * c, v.amateur ? T.data : T.text, 0.45f);
            else
                px_hatch(X(xa), Y(ry), (xb - xa) * c, rh * c, col, 0.42f, 0.55f * c, 0.2f * c);
            if (xb - xa >= 9.0f) {
                if (v.state == zones::kNotAllowed) snprintf(b, sizeof(b), "NOT ALLOWED");
                else if (v.state == zones::kIndoor) snprintf(b, sizeof(b), "INDOOR ONLY");
                else if (v.state == zones::kLicence) snprintf(b, sizeof(b), "LICENCE NEEDED");
                else if (v.amateur) snprintf(b, sizeof(b), "OK  -  AMATEUR");
                else if (v.cap_mw > 0) { char cb[16]; cap_text(v.cap_mw, cb, sizeof(cb)); snprintf(b, sizeof(b), "OK  -  %s MAX", cb); }
                else snprintf(b, sizeof(b), "OK");
                const float tw = px_text_w(b, 0.8f * c) / c;
                px_fill(X((xa + xb) * 0.5f - tw * 0.5f - 0.35f), Y(ry + 0.2f), (tw + 0.7f) * c, 1.3f * c, T.ground, 1.0f);
                txt(b, (xa + xb) * 0.5f, ry + 0.4f, 0.8f, v.state == zones::kAllowed && v.amateur ? T.data : col, 1);
            }
            r = e + 1;
        }
    }

    // AUTO: a striped slot of its own.
    {
        const float ax = slot_x(0);
        px_hatch(X(ax - bwid * 0.5f), Y(yT), bwid * c, (yB - yT) * c,
                 sc.auto_mode ? T.text : T.quiet, sc.auto_mode ? 0.45f : 0.2f, 0.6f * c, 0.25f * c);
        txt("AUTO", ax, yB + 0.7f, 0.85f, sc.auto_mode ? T.text : T.quiet, 1);
    }
    // The bars, from their glided levels.
    for (int r = 0; r < n; r++) {
        const int ci = order[r];
        const float x = slot_x(r + 1);
        const float lvl = chan_bar_[ci], pk = chan_peak_[ci];
        const float y = Yd(lvl), py_ = Yd(pk);
        const bool banned = rules && vd[r].state == zones::kNotAllowed;
        if (banned) px_hatch(X(x - sw * 0.5f), Y(yT), sw * c, (yB - yT) * c, kDanger, 0.16f, 0.55f * c, 0.2f * c);
        px_fill(X(x - bwid * 0.5f), Y(y), bwid * c, std::max(0.25f, yB - y) * c,
                banned ? T.quiet : busy_col[busy_level(lvl)], banned ? 0.4f : 0.92f);
        if (pk > lvl + 1.0f)
            px_fill(X(x - bwid * 0.5f - 0.08f), Y(py_ - 0.12f), (bwid + 0.16f) * c, 0.24f * c, T.text, banned ? 0.4f : 0.75f);
    }
    // The link's marker, gliding to its channel after a move.
    if (link_r >= 0 && chan_link_x_ >= 0.0f) {
        const float x = slot_xf(chan_link_x_);
        const int ci = order[link_r];
        const float y = std::min(Yd(chan_bar_[ci]), Yd(chan_peak_[ci]));
        px_fill(X(x - bwid * 0.5f - 0.1f), Y(Yd(chan_bar_[ci]) - 0.42f), (bwid + 0.2f) * c, 0.42f * c, T.accent, 1.0f);
        const float lw = px_text_w("LINK", 0.8f * c) / c + 0.8f;
        px_pill("LINK", X(x - lw * 0.5f), Y(y - 2.1f), 0.8f * c, T.accent, T.ground, 1.0f, false);
    }
    // Labels where they are read: band edges, band names, and the cursor's.
    for (int k = 0; k < nb; k++) {
        const int ends[2] = { band_lo[k], band_hi[k] };
        for (int e = 0; e < (ends[0] == ends[1] ? 1 : 2); e++) {
            if (std::fabs((float)(ends[e] + 1) - chan_cursor_x_) <= 2.5f) continue;
            snprintf(b, sizeof(b), "%d", sc.freq_mhz[order[ends[e]]]);
            txt(b, slot_x(ends[e] + 1), yB + 0.7f, 0.8f, T.quiet, 1);
        }
        const int lo = sc.freq_mhz[order[band_lo[k]]], hi = sc.freq_mhz[order[band_hi[k]]];
        snprintf(b, sizeof(b), "%.1f GHz", (lo + hi) * 0.5f / 1000.0f);
        txt(b, (slot_x(band_lo[k] + 1) + slot_x(band_hi[k] + 1)) * 0.5f, yB + 2.4f, 0.9f, T.quiet, 1);
    }
    // The cursor, gliding.
    {
        const float cx = slot_xf(chan_cursor_x_);
        px_fill(X(cx - sw * 0.62f), Y(yT - 0.5f), sw * 1.24f * c, (yB - yT + 2.5f) * c, T.accent, 0.07f);
        px_frame(X(cx - sw * 0.62f), Y(yT - 0.5f), sw * 1.24f * c, (yB - yT + 2.5f) * c, 0.16f * c, T.accent, 1.0f);
        if (cur > 0) {
            snprintf(b, sizeof(b), "%d", sc.freq_mhz[order[cur - 1]]);
            const float tw = px_text_w(b, 0.95f * c) / c;
            px_fill(X(cx - tw * 0.5f - 0.3f), Y(yB + 0.5f), (tw + 0.6f) * c, 1.3f * c, T.ground, 1.0f);
            txt(b, cx, yB + 0.65f, 0.95f, T.text, 1);
        }
    }
    // Where the cursor's rule comes from, small, at the foot.
    if (source && source[0]) {
        snprintf(b, sizeof(b), "Source: %s", source);
        txt(b, 97.8f, yB + 2.45f, 0.75f, T.quiet, 2);
    }
    px_flush();
    return moving;
}

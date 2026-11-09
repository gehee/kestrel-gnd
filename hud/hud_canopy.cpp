// Canopy HUD — the edge-anchored MEDIUM layout.
//
// The legacy HUD stacks labelled rows inward from each corner, which puts text
// across the middle third of the frame. This one anchors everything to the two
// screen edges and leaves the centre to the video: two headline figures, two
// segment tracks a side, a state row and an icon row.
//
// Geometry is written in the mockup's own 1200x620 pixel space and mapped to
// frustum units through U below, so the proportions here can be read against
// the design directly. X is measured inward from the panel's own screen edge
// (both panels use the same offsets, mirrored), Y upward from the bottom.

#include "hud/osd_detail.hpp"
#include "hud/osd.hpp"
#include "hud/settings.hpp"
#include "base/settings.hpp"
#include "hud/vec_icons.hpp"
#include "hud/hud_theme.hpp"
#include "render/software/canopy.hpp"
#include "base/time_util.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace {

// The palette is a theme now (see hud_theme.hpp), pulled in at the top of
// draw_canopy_hud. The names are kept because they say what each colour is FOR
// - white is type, cyan is a measured track, lime is live state - and those
// roles do not change between themes even when the hues do.
//
// The one that needs saying: the quiet grey is what a state word is drawn in
// when the state is the unremarkable one. DISARMED and IDLE share it
// deliberately - they are the same kind of statement on either side of the
// screen, and giving one of them a colour of its own would rank them against
// each other.

// The canopy's design numbers, its frame and its link animation are shared
// with the native Ascent's software HUD (render/software/canopy.hpp).
using canopy::kBleed;
using canopy::kSlantRad;
using canopy::CanopyFrame;
using canopy::canopy_frame;
using canopy::kWedgeX;
using canopy::kWedgeTop;
using canopy::kWedgeBot;
using canopy::kWedgeA;

// Space between a track's name and the track itself, in mockup pixels. This
// is the number the layout actually cares about; where the track starts falls
// out of it once the label has been measured.
constexpr float kLabelGap = 12.0f;

// Rest alpha for every rail line, all four of them, both panels. Was 0.95 -
// close enough to solid that the rails read brighter than the rest of the
// instrument at a glance.
constexpr float kRailAlpha = 0.8f;

// How far in from the screen edge the content starts. Was 28, which the mockup
// inherited from a 1200px canvas with wider margins all round; on the goggle it
// was giving away more of the frame than the layout needs.
constexpr float kEdge = 14.0f;

// How long the arm/disarm wipe takes, and how long ARMED holds the label slot
// before the flight timer takes it over.
constexpr float kWipeUs      = 900000.0f;
constexpr float kArmedHoldUs = 1800000.0f;
// How far inboard the frame line runs, in mockup pixels: exactly where the
// flat top rail ends and the arrowhead's angled edge begins in the outline
// documented below (M0 464 L252 456 L300 508 ...) - the rail has no business
// running past L252 456, which is a flat run, into the angled L300 508 edge.
// Was 272, overshooting that boundary by 20px and leaving a stub of rail
// drawn over what should be the angled tip instead - most visible as the
// bit of rail sitting past DISARMED/STANDBY that never quite lined up with
// the arrow. The wipe is measured across this, not per rule segment, so the
// front crosses the gap the state label sits in rather than restarting on
// the far side of it.
constexpr float kFrameEnd = 252.0f;

// Segment counts: the coarse "state" track and the fine "stress" one.
constexpr int kSegsMain = 20;
constexpr int kSegsThin = 12;     // the VIDEO track: range left, twelfths (osd.cpp)

// The slant, as a matrix rather than a transform threaded through every
// draw call: rotate in the panel's own plane about a pivot on its outer
// edge, then hand the result to the existing model/view/projection chain.
// Everything drawn under it - wedge, rules, blocks, glyph quads - takes the
// same angle by construction, which is the only way the frame and the
// content cannot disagree.
void slant_mvp(const float* model, const float* vp, float pivot_x, float pivot_y,
               float angle, math::Mat4& out) {
    math::Mat4 tneg, tpos, rz, a, b, m;
    memset(tneg, 0, sizeof(math::Mat4));
    tneg[0] = tneg[5] = tneg[10] = tneg[15] = 1.0f;
    memcpy(tpos, tneg, sizeof(math::Mat4));
    tneg[12] = -pivot_x; tneg[13] = -pivot_y;
    tpos[12] =  pivot_x; tpos[13] =  pivot_y;
    math::rotateZ(rz, angle);
    math::multiply(a, tneg, rz);    // to the pivot, then rotate
    math::multiply(b, a, tpos);     // and back
    math::multiply(m, b, model);    // the slant sits inside the panel yaw
    math::multiply(out, m, vp);
}


}  // namespace

void OSD::draw_canopy_hud(const CanopyIn& in) {
    const HudTheme& TH = hud_theme_current();
    // About 1.2 px (half the frustum height is 540 px at 1080p).
    static const bool feather = settings_.get(hud::setting::hud_feather);
    canvas_->set_feather(feather ? in.frustum_h / 540.0f * 1.2f : 0.0f);
    const float kWhiteR = TH.text[0],   kWhiteG = TH.text[1],   kWhiteB = TH.text[2];
    const float kCyanR  = TH.data[0],   kCyanG  = TH.data[1],   kCyanB  = TH.data[2];
    const float kLimeR  = TH.accent[0], kLimeG  = TH.accent[1], kLimeB  = TH.accent[2];
    const float kShadeR = TH.ground[0], kShadeG = TH.ground[1], kShadeB = TH.ground[2];
    const float kIdleR  = TH.quiet[0],  kIdleG  = TH.quiet[1],  kIdleB  = TH.quiet[2];

    // Temperature is the one figure with a real physical ceiling, so it gets
    // a colour of its own rather than the theme's - a warning has to read the
    // same on every theme, or it stops being a warning. Amber then red, not
    // themed: this is a safety signal, not a hue that changes with taste.
    // Thresholds: canopy::temperature_alert (70/85 C), shared with the Ascent.
    auto temp_rgb = [&](float c, float base_r, float base_g, float base_b,
                        float& r, float& g, float& b) {
        const int alert = canopy::temperature_alert(c);
        if (alert == 2)      { r = 1.0f; g = 0.30f; b = 0.25f; }
        else if (alert == 1) { r = 1.0f; g = 0.72f; b = 0.15f; }
        else                 { r = base_r; g = base_g; b = base_b; }
    };

    // The goggle's own supply, per cell: canopy::supply_cells infers the pack,
    // canopy::cell_alert says when it is low (3.5 / 3.3 V a cell).
    auto cells_for = [](float v) { return canopy::supply_cells(v); };
    auto volt_rgb = [&](float per_cell, float base_r, float base_g, float base_b,
                        float& r, float& g, float& b) {
        const int alert = canopy::cell_alert(per_cell);
        if (alert == 2)      { r = 1.0f; g = 0.30f; b = 0.25f; }
        else if (alert == 1) { r = 1.0f; g = 0.72f; b = 0.15f; }
        else                 { r = base_r; g = base_g; b = base_b; }
    };

    // Panel height in real pixels, so the vector icons can be rasterized at
    // the size they are actually drawn rather than upsampled from a fixed one.
    const int screen_px_h = (dev && dev->has_output())
                          ? (int)dev->mode().h : 1080;
    const float s = in.s;

    // The panels' frame in mockup pixels; canopy_frame() says why each axis
    // is mapped the way it is.
    const CanopyFrame F = canopy_frame(in.frustum_w, in.frustum_h, in.overscan, s, wrap_depth);
    const float sd = F.sd, UX = F.UX, U = F.U, LX = F.LX;
    auto MY = [&](float y) { return F.MY(y); };

    // The highest thing the blades draw. Published because the menu has to
    // sit clear of it, and it moves: the panels are laid out in mockup pixels
    // scaled by the UI setting, so a bigger HUD pushes this up the screen.
    // A menu with a constant margin baked in is only ever right at 1.0x.
    canopy_top_y_ = MY(456.0f);
    auto LXo = [&](float x) { return F.LXo(x); };
    auto RXo = [&](float x) { return F.RXo(x); };

    const uint64_t now_us = get_time_us();

    // Whether there is an aircraft was decided - and settled against a
    // flickering link - where the top strip needed the same answer. Reading the
    // member rather than re-deriving it is what keeps the clock from vanishing
    // a beat before the panels arrive.
    const bool have_aircraft = hud_connected_;

    // Three states, and the whole HUD hangs off which one we are in.
    //
    //   IDLE_1  never linked this boot. The panels are not drawn at all and
    //           the clock has the screen. That emptiness is what gives the
    //           first sweep something to mean.
    //   CONNECTED
    //           panels live at full brightness.
    //   IDLE_2  linked once, not linked now. The panels stay, dimmed, holding
    //           the last reading with IDLE on the rail - the last numbers
    //           dimmed are worth more than a blank screen, and furniture
    //           vanishing mid-flight reads as a fault in the goggle rather
    //           than in the link.
    //
    // Both entries into CONNECTED replay the same rebuild sweep. IDLE_2 could
    // have had a gentler treatment of its own - relighting panels that are
    // already on screen - but one gesture for "the link is back" is worth more
    // than two that have to be told apart mid-flight, and the settle timer on
    // the rising edge is what stops a blinking link from replaying it.
    //
    // Leaving CONNECTED, in three overlapping beats, and the rebuild after it:
    // canopy::link_visual, which the Ascent's software HUD draws too.
    // Everything below reads these rather than branching on have_aircraft, so
    // the settled idle panel and the last frame of the transition are the same
    // drawing. Past the sweep every factor is 1 and costs nothing, which is
    // the state IDLE_2 sits in - dimmed by idle_dim, not by these.
    const canopy::LinkVisual link_v =
        canopy::link_visual(have_aircraft, now_us, canopy_link_prev_, canopy_link_up_, canopy_link_down_);
    if (!link_v.visible) return;             // IDLE_1
    // Nothing else need be driving repaints while the panels move.
    if (link_v.animating) signal_render(prof::kWakeAnim);
    const float idle_dim = link_v.idle_dim, rail_dim = link_v.rail_dim, mark = link_v.mark;
    const float f_rule = link_v.rule, f_shade = link_v.shade, f_block = link_v.block;

    // Text sizes, in the same units canvas_->text() takes elsewhere in the HUD.
    const float T_BIG   = canopy::kTextBig   * sd;
    const float T_UNIT  = canopy::kTextUnit  * sd;
    const float T_SMALL = canopy::kTextSmall * sd;
    const float T_ROW   = canopy::kTextRow   * sd;
    const float T_LABEL = canopy::kTextLabel * sd;
    const float T_STATE = canopy::kTextState * sd;

    // canvas_->text() places the BOTTOM of the glyph box at y, and the cairo
    // surface behind it draws the baseline at row 48 of 64 - a quarter of the
    // way up. So two sizes drawn at one y do NOT share a baseline: the smaller
    // sits low by a quarter of the difference, which is what made the current
    // read as a separate line from the voltage rather than part of it.
    // BL() converts a baseline to the y draw_text wants, for any size.
    auto BL = [](float baseline, float size) { return baseline - 0.25f * size; };

    // ...and the same problem bit the icons. canvas_->icon() takes the bottom of
    // its quad, so an icon given the text's y sat level with the bottom of the
    // glyph BOX - descender space included - and therefore low against the
    // letters. IY() centres an icon on the text's box instead, which is what
    // makes VRX 52C and its goggles finally sit on one line.
    auto IY = [](float baseline, float size, float ih) {
        return baseline + 0.25f * size - ih * 0.5f;
    };

    // The left panel is the aircraft, so it dims when the flight controller
    // stops talking - not when video stops. The two usually go together, but
    // stale telemetry drawn at full strength is the failure worth avoiding.
    const bool bf_fresh = in.bf.stamp_us != 0 &&
                          (get_time_us() - in.bf.stamp_us) < 2000000ULL;
    const float dim = (bf_fresh ? 1.0f : 0.75f) * f_block * idle_dim;
    char buf[64];

    // Units sit a step below the value they belong to - the figure is what you
    // read, the unit only says what it is in. Applies to each panel's headline
    // row (V and A here, km/h and m opposite), which were already doing this
    // but each with its own grey; they share one step now. Deliberately NOT
    // carried into the icon rows below - dimming units down there just made
    // them look muddy, and those rows read better all one white.
    const float kUnitStep = 0.72f;
    const float ur = kWhiteR * dim * kUnitStep,
                ug = kWhiteG * dim * kUnitStep,
                ub = kWhiteB * dim * kUnitStep;

    // How far to step past a number, measured as if every digit were a 0.
    // The face is proportional - 1 is narrower than 0 - so advancing by the
    // real width moved everything downstream of a reading each time a digit
    // changed: the bullet, the flight timer that lines up on it, the current
    // and its A all shuffled while the pack voltage drifted. Stepping by the
    // all-zeroes width holds them still, and still reflows properly when the
    // number actually gains or loses a digit.
    auto num_adv = [&](const char* sN, float size) {
        char ref[32];
        size_t n = strlen(sN);
        if (n >= sizeof(ref)) return canvas_->text_width(sN, size);
        for (size_t i = 0; i < n; i++)
            ref[i] = isdigit((unsigned char)sN[i]) ? '0' : sN[i];
        ref[n] = 0;
        return canvas_->text_width(ref, size);
    };

    // Arm state colours the rules, and changing it is not instant: the new
    // colour is drawn along the frame from one end, and back the other way on
    // disarm. A state that costs a real-world action to reach should look like
    // it took one - and the direction is the difference between "this just
    // became live" and "this just stood down".
    //
    // Unknown (nothing said either way) draws the disarmed colour but no
    // label, so we never assert an arm state we did not read.
    const bool armed = (in.bf.arm == 1);
    if (in.bf.arm != canopy_arm_prev_) {
        // The first reading settles rather than animating - the frame should
        // not wipe itself in every time the goggle boots or the link returns.
        canopy_arm_change_ = (canopy_arm_prev_ == -2) ? 0 : now_us;
        canopy_arm_prev_   = in.bf.arm;
    }
    float wipe = 1.0f;
    if (canopy_arm_change_) {
        wipe = (float)(now_us - canopy_arm_change_) / kWipeUs;
        wipe = fmaxf(0.0f, fminf(1.0f, wipe));
        // Keep the frames coming for the length of the wipe. The OSD otherwise
        // repaints only when something asks it to, and an animation nobody is
        // driving would advance in whatever steps the video happened to give.
        if (wipe < 1.0f) signal_render(prof::kWakeAnim);
    }
    // Colours for the state being entered and the one being left. Cool
    // slate at rest, everywhere - DISARMED, IDLE and STANDBY all read the
    // same rail colour now instead of three different greys, and it sits
    // back further than plain white did (which read as too much against
    // most video) without going all the way back to the old muddy grey.
    // The one colour that actually means something, the accent, is
    // reserved for ARMED so it still stands out as the state change that
    // matters. See kRestR/G/B below - named so STANDBY's text can match it
    // without repeating the literal.
    constexpr float kRestR = 0.663f, kRestG = 0.706f, kRestB = 0.741f;
    const float nc[3] = { armed ? kLimeR : kRestR, armed ? kLimeG : kRestG,
                          armed ? kLimeB : kRestB };
    const float oc[3] = { armed ? kRestR : kLimeR, armed ? kRestG : kLimeG,
                          armed ? kRestB : kLimeB };
    // Arming draws from the screen edge inward; disarming runs back the other
    // way, from the tip out. The front travels the frame's whole span, bleed
    // included - stopping it at 0 would leave the off-screen run of every rule
    // painted in the colour of the state we just left.
    const float front = armed ? -kBleed + (kFrameEnd + kBleed) * wipe
                              :  kFrameEnd - (kFrameEnd + kBleed) * wipe;
    float rr = nc[0], rg = nc[1], rb = nc[2];

    // One rule segment, split where the wipe front currently sits. The segment
    // knows its own place along the frame, so a rule broken around the state
    // label still animates as one line.
    auto rule = [&](bool left, float xa, float xb, float y, float th,
                    float alpha) {
        auto X = [&](float v) { return left ? LXo(v) : RXo(v); };
        // Alpha is a function of position along the FRAME, not of how far
        // along its own segment a piece happens to be: flat for the run in
        // from the screen edge, then a quick fade to nothing over the last
        // fifth, so every rail dies into the arrow's angled edge instead of
        // stopping dead on it. Per-segment ramps could not express that - a
        // segment that straddles the fade point would stretch the ramp over
        // its whole length - which is why the split below exists.
        constexpr float kFadeFrac  = 0.5f;
        const float fade_start = kFrameEnd * (1.0f - kFadeFrac);
        auto A = [&](float v) {
            float a = alpha;
            if (v > fade_start) {
                float t = (v - fade_start) / (kFrameEnd - fade_start);
                t = fmaxf(0.0f, fminf(1.0f, t));
                // Runs from halfway along the rail out to the tip, easing
                // rather than dropping: a cubic over a short zone read as the
                // line snapping off, and a straight ramp read as it changing
                // colour. Quadratic over half the rail stays bright where the
                // content is and thins out gradually into the arrow.
                a = alpha * (1.0f - t * t);
            }
            return a * rail_dim;
        };
        // The link sweep clips the whole frame line to how far it has run in,
        // measured across the frame rather than per segment, so a rule broken
        // around the state label grows as one line.
        float reach = kFrameEnd * f_rule;
        if (xa >= reach) return;
        if (xb > reach) xb = reach;
        float cut = fminf(fmaxf(front, xa), xb);
        const float* head = armed ? nc : oc;   // the end nearer the screen edge
        const float* tail = armed ? oc : nc;
        // Walk the span, breaking it at the wipe front and at steps through
        // the fade zone. The front split keeps each piece on one side of the
        // state colour change; the fade steps are what make the curve above
        // visible at all - draw_fade_rule takes one alpha per end and
        // interpolates straight between them, so a single piece spanning the
        // whole fade zone would draw a linear ramp no matter what A() says.
        constexpr int kFadeSteps = 10;
        float b[3 + kFadeSteps + 1];
        int n = 0;
        b[n++] = xa;
        b[n++] = xb;
        b[n++] = cut;
        for (int i = 0; i <= kFadeSteps; i++)
            b[n++] = fade_start + (kFrameEnd - fade_start) * (float)i / (float)kFadeSteps;
        for (int i = 0; i < n; i++) b[i] = fminf(fmaxf(b[i], xa), xb);
        for (int i = 1; i < n; i++) {            // insertion sort, n is tiny
            float k = b[i];
            int j = i - 1;
            while (j >= 0 && b[j] > k) { b[j + 1] = b[j]; j--; }
            b[j + 1] = k;
        }
        for (int i = 0; i + 1 < n; i++) {
            const float s = b[i], e = b[i + 1];
            if (e - s <= 1e-4f) continue;
            const float* c = (0.5f * (s + e) < cut) ? head : tail;
            canvas_->fade_rule(X(s), y, X(e), y, th, A(s), c[0], c[1], c[2], A(e));
        }
    };

    math::Mat4 mvp_ls, mvp_rs;
    slant_mvp(in.model_l, in.vp, LXo(56.0f), MY(502.0f),  kSlantRad, mvp_ls);
    slant_mvp(in.model_r, in.vp, RXo(56.0f), MY(502.0f), -kSlantRad, mvp_rs);

    // ---------------- LEFT PANEL — the aircraft ----------------
    canvas_->set_transform(mvp_ls);

    // Mockup outline, both panels: M0 464 L252 456 L300 508 L252 572 L0 610 Z,
    // with the shLE gradient (.66 at the edge, .34 half way, 0 at the point)
    // sampled at the same columns.
    // Mockup 2's ground: M0 464 L252 456 L300 508 L252 572 L0 610 Z. The body
    // holds very nearly full height for its whole length and then throws a
    // short arrowhead out to the point - it is a blade with a tip, not a
    // wedge. Tapering it gradually (which is what this was) draws a different
    // object: the rails start closing while they are still carrying content,
    // so they cut across the angle the type sits on.
    //
    // Both rails are therefore FLAT in panel space, and the slant matrix gives
    // them exactly the angle of the text. Only the last 48px is the tip.
    //
    // It starts outboard of the screen edge so the ground bleeds off rather
    // than showing a vertical seam inside the overscan margin.
    // The wedge's outline and alpha: canopy::kWedge* (shared with the Ascent).
    {
        float xs[4], tops[4], bots[4];
        for (int i = 0; i < 4; i++) {
            xs[i]   = LXo(kWedgeX[i]);
            tops[i] = MY(kWedgeTop[i]);
            bots[i] = MY(kWedgeBot[i]);
        }
        float wa[4];
        for (int i = 0; i < 4; i++) wa[i] = kWedgeA[i] * f_shade * idle_dim;
        canvas_->fade_wedge(xs, tops, bots, wa, 4, kShadeR, kShadeG, kShadeB);
    }

    // Where the state label sits: level with the current figure in the
    // headline, which the headline itself works out. Until it has, fall back to
    // a fixed offset - the label is drawn after it now.
    float state_x = LXo(kEdge + 46.0f);

    // Recording marker. It sits above the panel's top rule rather than in a
    // row, because it is a state of this box and not a reading from anything.
    if (in.rec_active) {
        float x = LXo(kEdge), y = MY(440.0f);
        float d = 9.0f * U;
        canvas_->poly({{x, y}, {x + d, y}, {x + d, y + d}, {x, y + d}},
                  1.0f, 0.95f, 0.15f, 0.15f);
        canopy::recording_label(buf, sizeof(buf), in.rec_secs);
        canvas_->text(buf, x + d + 8.0f * U, MY(442.0f), T_ROW, false, 1.0f, 0.35f, 0.35f);
    } else if (in.rec_failed[0]) {
        // Why REC went off by itself, where it was.
        float x = LXo(kEdge), y = MY(440.0f);
        float d = 9.0f * U;
        canvas_->poly({{x, y}, {x + d, y}, {x + d, y + d}, {x, y + d}},
                  1.0f, 0.95f, 0.15f, 0.15f);
        canvas_->text(in.rec_failed, x + d + 8.0f * U, MY(442.0f), T_ROW, false, 1.0f, 0.35f, 0.35f);
    }

    // Headline: voltage, then current in the small slot. Voltage is per cell by
    // default — the same number on any pack size, where a pack figure means
    // nothing until you know the cell count.
    {
        float x    = LXo(kEdge);
        float base = MY(512.0f) + 0.25f * T_BIG;   // one baseline for the row
        bool have_v = (volt_mode == 1) ? in.bf.have_pack_v : in.bf.have_cell_v;
        float vv    = (volt_mode == 1) ? in.bf.pack_v      : in.bf.cell_v;
        if (have_v) {
            snprintf(buf, sizeof(buf), "%.2f", vv);
            canvas_->text(buf, x, BL(base, T_BIG), T_BIG, false,
                      kWhiteR * dim, kWhiteG * dim, kWhiteB * dim);
            x += num_adv(buf, T_BIG);
            canvas_->text("V", x, BL(base, T_UNIT), T_UNIT, false, ur, ug, ub);
            x += canvas_->text_width("V", T_UNIT) + 10.0f * U;
        } else {
            canvas_->text("--", x, BL(base, T_BIG), T_BIG, false, 0.45f, 0.45f, 0.45f);
            x += canvas_->text_width("--", T_BIG) + 10.0f * U;
        }
        if (in.bf.have_amps) {
            canvas_->text("\xE2\x80\xA2", x, BL(base, T_UNIT), T_UNIT, false,
                      dim * 0.42f, dim * 0.42f, dim * 0.42f);
            x += canvas_->text_width("\xE2\x80\xA2", T_UNIT) + 10.0f * U;
            state_x = x;                            // the state label lines up here
            snprintf(buf, sizeof(buf), "%.1f", in.bf.amps);
            canvas_->text(buf, x, BL(base, T_UNIT), T_UNIT, false,
                      dim * 0.82f, dim * 0.82f, dim * 0.82f);
            x += num_adv(buf, T_UNIT);
            canvas_->text("A", x, BL(base, T_SMALL), T_SMALL, false, ur, ug, ub);
        }
    }

    // Top rule, interrupted by the state label. Disarmed says so; armed hands
    // the slot to the flight timer once the transition has been seen.
    {
        // DISARMED names the state; on arming it becomes ARMED just long
        // enough to be read, and then the flight timer takes the slot over -
        // by which point the colour of the line is saying the same thing.
        const char* tag = nullptr;
        char tagbuf[16];
        bool holding = canopy_arm_change_ &&
                       (now_us - canopy_arm_change_) < (uint64_t)kArmedHoldUs;
        if (armed && in.bf.have_timer && !holding) {
            snprintf(tagbuf, sizeof(tagbuf), "%d'%02d\"",
                     in.bf.timer_s / 60, in.bf.timer_s % 60);
            tag = tagbuf;
        } else if (armed) {
            tag = "ARMED";
        } else if (in.bf.arm == 0) {
            tag = "DISARMED";
        }
        float y = MY(456.0f);
        if (tag) {
            // The label sits in a gap in the rule. Its width is measured, so
            // the second segment starts where the text actually ends.
            // Aligned with the current figure above it rather than tucked
            // against the voltage, which put it far closer to the big number
            // than to anything it belongs with.
            float lx    = (state_x - LX) / UX;       // back to design units
            float tw_px = canvas_->text_width(tag, T_STATE) / UX;
            float gap_a = lx - 8.0f, gap_b = lx + tw_px + 10.0f;
            rule(true, -kBleed, gap_a, y, 2.2f * U, kRailAlpha);
            if (gap_b < kFrameEnd)
                rule(true, gap_b, kFrameEnd, y, 2.2f * U, kRailAlpha);
            // The label takes the colour of the state it names, and only once
            // the wipe has reached it - it is part of the line, not a caption
            // beside it.
            const float* lc = (front >= gap_a) == armed ? nc : oc;
            // Sits centred on the rail it interrupts, so a taller label grows
            // both ways rather than drifting off the line.
            // Centred on the rail: half the glyph height sits below the line,
            // half above, so growing the label does not walk it off the rule.
            float half_px = (T_STATE / U) * 0.5f;
            canvas_->text(tag, LXo(lx), MY(456.0f + half_px), T_STATE, false, lc[0], lc[1], lc[2]);
        } else {
            // No label to interrupt it (no flight controller talking): the rail runs the
            // full length, to the same boundary as the other rails and the right panel's,
            // rather than stopping short where the label would have been.
            rule(true, -kBleed, kFrameEnd, y, 2.2f * U, kRailAlpha);
        }
        // Same weight and brightness as the top rail. It was a 1.2px hairline at
        // 0.42 - a deliberate accent - but next to a 2.2px rule at 0.95 it read
        // as an afterthought rather than as the other edge of the same blade.
        // Ends at kFrameEnd, same boundary the top rail stops at - was 224,
        // short of the L252 572 vertex where the bottom angled edge begins.
        rule(true, -kBleed, kFrameEnd, MY(596.0f), 2.2f * U, kRailAlpha);
        // (Removed) A short diagonal used to sit across the shoulder of the
        // arrowhead. It was the mockup's corner accent and the only mark in the
        // frame not parallel to everything else - which was the argument for it
        // and, on screen, the argument against: it sat nearest the centre,
        // where it read as a stray mark over the video rather than as part of
        // the blade. Two rails, and the ground's own tip, carry the shape.
    }

    // Track 1 — cell level, 3.30V empty to 4.20V full.
    // Track 2 — sag: how far the pack is below the highest cell voltage seen
    // this session. It is the same reading as the headline, shown as stress
    // rather than state, and it is only meaningful next to the current draw.
    {
        static float cell_peak = 0.0f;
        float cell = in.bf.have_cell_v ? in.bf.cell_v
                   : (in.bf.have_pack_v ? 0.0f : 0.0f);
        float lvl = 0.0f, sag = 0.0f;
        if (cell > 2.5f) {
            if (cell > cell_peak) cell_peak = cell;
            lvl = (cell - 3.30f) / (4.20f - 3.30f);
            lvl = fmaxf(0.0f, fminf(1.0f, lvl));
            // 0.5V of droop is the whole track; that is a hard pull on a tired
            // pack and about the point a 4S starts hitting its low-voltage cut.
            sag = fmaxf(0.0f, fminf(1.0f, (cell_peak - cell) / 0.50f));
        }
        // Track names sit at the same strength as the track they name - they
        // are part of the gauge, not a caption under it, and held back at
        // 0.55/0.62 they read as disabled next to a lit bar.
        canvas_->text("CELL", LXo(kEdge), MY(527.0f), T_LABEL, false,
                  kWhiteR * dim, kWhiteG * dim, kWhiteB * dim);
        // The track starts a fixed gap after the widest of the two names, not
        // at a hard-coded inset. The inset was a guess at how wide the labels
        // would render, and this font is condensed and unspaced where the
        // design's was not - so the guess left a third of the panel empty
        // between a name and the thing it names. Measure instead.
        float lab_w = fmaxf(canvas_->text_width("CELL", T_LABEL), canvas_->text_width("SAG", T_LABEL));
        float tx0   = LXo(kEdge) + lab_w + kLabelGap * UX;
        float tw    = LXo(271.0f) - tx0;
        canvas_->seg_track(tx0, MY(525.0f), tw, 9.0f * U, kSegsMain,
                       lvl, true, kCyanR, kCyanG, kCyanB, 0.70f * dim, 1.00f * dim,
                       f_block);
        canvas_->text("SAG", LXo(kEdge), MY(537.0f), T_LABEL, false,
                  kLimeR * dim, kLimeG * dim, kLimeB * dim);
        canvas_->seg_track(tx0, MY(534.0f), tw, 4.0f * U, kSegsMain,
                       sag, true, kLimeR, kLimeG, kLimeB, 0.60f * dim, 1.00f * dim,
                       f_block);
    }

    // The two bottom rows are read by whose reading they are: the aircraft
    // first, this box second. Each opens with the icon that says which, so the
    // eye can find "what is the drone doing" without reading the values.
    const float ih   = 14.0f * U;          // icon height, scales with the rows
    // The icons keep their own proportions, so the drone (square) and the
    // goggle (wide) cannot share a width. They share a *slot* instead, sized
    // to the widest of them, with each icon centred in it - that is what keeps
    // the text after them on one left edge down both rows.
    const float iw   = ih * 1.35f;
    const float igap = 8.0f * UX;
    // Rasterize at the size we are about to draw. ih is in frustum units, and
    // the frustum's full height is the panel's full height in pixels.
    const int   ipx  = (in.frustum_h > 0.0f)
                     ? (int)lroundf(ih / (in.frustum_h * 2.0f) * (float)screen_px_h)
                     : 24;
    // Draw an icon centred in the slot whose left edge is x, without stretching
    // it: whichever of width or height binds first sets the scale.
    auto icon = [&](VecIcon which, float x, float base,
                    float r, float g, float b) {
        float asp = 1.0f;
        render::ImageId t = vec_icon_texture(*canvas_, which, ipx, &asp);
        if (!t) return;
        float h = ih, w = ih * asp;
        if (w > iw) { w = iw; h = iw / asp; }
        canvas_->icon(t, x + (iw - w) * 0.5f, IY(base, T_ROW, h), w, h, r, g, b);
    };
    // Icon and label are one object, so they share a colour - and that colour
    // is the theme's text white, the same one the voltage reading above uses.
    // They used to be two different dimmed tints (a 0.8/0.85/0.9 icon and a
    // flat 0.62 grey label), which read as muddy next to a white number on
    // the same panel.
    const float ir = kWhiteR * dim, ig = kWhiteG * dim, ib = kWhiteB * dim;
    const float tr = ir, tg = ig, tb = ib;

    // Drone row: what the aircraft is doing, and how far away it is doing it.
    {
        float base = MY(560.0f) + 0.25f * T_ROW;
        float x    = LXo(kEdge);
        icon(VecIcon::Drone, x, base, ir, ig, ib);
        x += iw + igap;
        if (in.bf.have_mode) {
            canvas_->text(in.bf.mode, x, BL(base, T_ROW), T_ROW, false, tr, tg, tb);
            x += canvas_->text_width(in.bf.mode, T_ROW) + igap;
        }
        if (in.dist_m >= 0) {
            canvas_->text("\xE2\x80\xA2", x, BL(base, T_ROW), T_ROW, false,
                      tr * 0.68f, tg * 0.68f, tb * 0.68f);
            x += canvas_->text_width("\xE2\x80\xA2", T_ROW) + igap;
            icon(VecIcon::Distance, x, base, ir, ig, ib);
            x += iw + igap;
            if (in.dist_m >= 1000) snprintf(buf, sizeof(buf), "%.2fkm", in.dist_m / 1000.0f);
            else                        snprintf(buf, sizeof(buf), "%dm", in.dist_m);
            canvas_->text(buf, x, BL(base, T_ROW), T_ROW, false, tr, tg, tb);
        }
    }

    // Goggle row: this unit's own SoC temperature. The barrel-jack battery
    // belongs beside it, but nothing reads that ADC yet (the baseband's
    // PRJ_CMD_GET_ADC_VALUE path is unwired), so the slot stays empty rather
    // than showing an invented voltage.
    {
        float base = MY(584.0f) + 0.25f * T_ROW;
        float x    = LXo(kEdge);
        icon(VecIcon::Goggles, x, base, ir, ig, ib);
        x += iw + igap;
        snprintf(buf, sizeof(buf), "VRX %.0fC", in.vrx_temp_c);
        float vr, vg, vb;
        temp_rgb(in.vrx_temp_c, tr, tg, tb, vr, vg, vb);
        canvas_->text(buf, x, BL(base, T_ROW), T_ROW, false, vr, vg, vb);
        x += canvas_->text_width(buf, T_ROW);              // the end of what is drawn so far

        // Its own TX power (the uplink), as the radio pushes it.
        if (in.vrx_pwr[0]) {
            snprintf(buf, sizeof(buf), " \xE2\x80\xA2 %s", in.vrx_pwr);
            canvas_->text(buf, x, BL(base, T_ROW), T_ROW, false, tr, tg, tb);
            x += canvas_->text_width(buf, T_ROW);
        }

        // The goggle's own supply, beside its temperature - the two readings
        // that are about this unit rather than the aircraft. Drawn only when
        // the ADC has actually produced a value; an unwired or failed read
        // leaves the slot empty instead of showing a plausible-looking zero.
        if (in.vrx_volts > 0.5f) {
            x += igap * 1.6f;
            const int   n  = cells_for(in.vrx_volts);
            const float pc = n ? in.vrx_volts / (float)n : 0.0f;
            float br, bg, bb;
            volt_rgb(pc, tr, tg, tb, br, bg, bb);
            snprintf(buf, sizeof(buf), "%.1fV", in.vrx_volts);
            canvas_->text(buf, x, BL(base, T_ROW), T_ROW, false, br, bg, bb);
            x += canvas_->text_width(buf, T_ROW);
        }

        // The goggle's WiFi access point on: a 2.4 GHz transmitter beside the
        // RC link, and the web page open to anyone in range - worth seeing.
        // In the theme's accent, the colour of live state.
        if (in.wifi_on) {
            x += igap * 1.6f;
            canvas_->text("WIFI", x, BL(base, T_ROW), T_ROW, false, kLimeR * dim, kLimeG * dim, kLimeB * dim);
        }
    }

    // ---------------- RIGHT PANEL — the links ----------------
    canvas_->set_transform(mvp_rs);

    // idle_dim already carries the link state, and it is the deeper of the
    // two - stacking the old 0.75 on top of it only made the right panel
    // darker than the left for the same condition.
    const float ldim = idle_dim * f_block;
    const float lur = kWhiteR * ldim * kUnitStep,
                lug = kWhiteG * ldim * kUnitStep,
                lub = kWhiteB * ldim * kUnitStep;

    {
        float xs[4], tops[4], bots[4];
        for (int i = 0; i < 4; i++) {
            xs[i]   = RXo(kWedgeX[i]);
            tops[i] = MY(kWedgeTop[i]);
            bots[i] = MY(kWedgeBot[i]);
        }
        float wa[4];
        for (int i = 0; i < 4; i++) wa[i] = kWedgeA[i] * f_shade * idle_dim;
        canvas_->fade_wedge(xs, tops, bots, wa, 4, kShadeR, kShadeG, kShadeB);
    }
    // STANDBY's geometry, worked out here rather than down in the rail block
    // because the reveal below is derived from it. lx puts STANDBY's first
    // letter the same distance from the wedge's corner point (kWedgeX[3],
    // design x=300, where the background fade tapers to nothing) as
    // DISARMED's last letter sits from that same corner on the left.
    //
    // Anchored to DISARMED's STABLE fallback position (kEdge + 46, the same
    // constant state_x defaults to before the current reading overwrites it -
    // LXo(v) and (.-LX)/UX are inverses, so it reduces to the literal), NOT
    // the live state_x: that tracks the current figure's own width, which
    // changes with every digit, and reusing it here made STANDBY's position
    // jitter across a 65+ unit range frame to frame.
    const char* kStandbyTag = "STANDBY";
    constexpr float kCornerX = 300.0f;
    const float sb_disarmed_end = (kEdge + 46.0f) + canvas_->text_width("DISARMED", T_STATE) / UX;
    const float sb_tw_px = canvas_->text_width(kStandbyTag, T_STATE) / UX;
    const float sb_lx    = sb_disarmed_end - sb_tw_px;

    // Drives STANDBY's wipe-reveal (draw_text_wipe, below): unlike ARMED/
    // DISARMED, nothing else on this panel animates when vtx_low_power
    // toggles mid-connection, so it needs its own timer rather than riding
    // on f_rule (which has already finished by then) or the arm wipe
    // (a different state entirely).
    if (!canopy_standby_seen_) {
        canopy_standby_seen_ = true;
        canopy_standby_prev_ = in.vtx_low_power;
    } else if (in.vtx_low_power != canopy_standby_prev_) {
        canopy_standby_change_ = now_us;
        canopy_standby_prev_   = in.vtx_low_power;
    }
    float standby_reveal = canopy_standby_prev_ ? 1.0f : 0.0f;
    if (canopy_standby_change_) {
        float p = (float)(now_us - canopy_standby_change_) / kWipeUs;
        p = fmaxf(0.0f, fminf(1.0f, p));
        // Ride a front with the same span and rate as the arm wipe's `front`
        // rather than spreading the whole kWipeUs across the word: kWipeUs
        // buys the colour front the full -kBleed..kFrameEnd frame, so
        // stretching the same time over a ~107-unit word made the letters
        // sweep at a third of its speed. Deriving the reveal from where that
        // front currently sits means the two move together, and cross the
        // word at the same moment and the same place. Appearing rides the
        // outward sweep, leaving the inward one, which is the direction the
        // letters already had.
        const float span   = kFrameEnd + kBleed;
        const float sfront = in.vtx_low_power ? kFrameEnd - span * p
                                              : -kBleed   + span * p;
        standby_reveal = (sb_lx + sb_tw_px - sfront) / sb_tw_px;
        standby_reveal = fmaxf(0.0f, fminf(1.0f, standby_reveal));
        if (p < 1.0f) signal_render(prof::kWakeAnim);
    }
    // Top rule, interrupted by IDLE exactly the way the left one is broken
    // around DISARMED. Same slot, same weight, same centring on the rail - the
    // two panels are one instrument and a state word should look the same on
    // either side of it. Only the colour differs, and only because the states
    // do: DISARMED is a thing the aircraft is, IDLE is a thing that is missing.
    {
        float y = MY(456.0f);
        if (mark > 0.0f) {
            const char* tag = "IDLE";
            // Fixed, where the left panel's label lands. It cannot track the
            // distance figure the way DISARMED tracks the current, because
            // there is no distance reading when the link is down - which is
            // the only time this label is drawn.
            const float lx = 150.0f;
            float tw_px = canvas_->text_width(tag, T_STATE) / UX;
            // The gap opens from the middle of where the word will be, so the
            // rail parts around the label rather than the label appearing in a
            // hole that was already waiting for it.
            float mid   = lx + tw_px * 0.5f;
            float gap_a = mid + (lx - 8.0f - mid) * mark;
            float gap_b = mid + (lx + tw_px + 10.0f - mid) * mark;
            float end   = 200.0f + (kFrameEnd - 200.0f) * mark;
            rule(false, -kBleed, gap_a, y, 2.2f * U, kRailAlpha);
            if (gap_b < end)
                rule(false, gap_b, end, y, 2.2f * U, kRailAlpha);
            // Same grey DISARMED uses. Scaled by mark alone rather than by the
            // panel dim: the label is the reason the panel is dim, so fading it
            // with everything else would hide the answer along with the
            // question. draw_text has no alpha, so this is the fade.
            float half_px = (T_STATE / U) * 0.5f;
            canvas_->text(tag, RXo(lx), MY(456.0f + half_px), T_STATE, true,
                      kIdleR * mark, kIdleG * mark, kIdleB * mark);
        } else if (standby_reveal > 0.0f) {
            // Same slot IDLE uses, for the one other state worth breaking
            // the rail for: the VTX itself reporting it is radiating at
            // standby power right now, not the air's separate (and
            // confirmed unreliable - see ar8030-power-and-standby-verified)
            // enable/disable setting.
            //
            // The gap itself is fixed-size from the first frame, same as
            // DISARMED - only the word wipes into it, left letter first,
            // using standby_reveal computed above. draw_text_wipe crops the
            // same cached texture draw_text would use, so this costs
            // nothing extra once the string has been rendered once.
            //
            // Position and width were worked out above, alongside the timer
            // that drives standby_reveal - both need them.
            const char* tag  = kStandbyTag;
            const float lx    = sb_lx;
            const float tw_px = sb_tw_px;
            // The gap tracks how much of the word is actually on screen, so
            // the rail closes in behind the letters as they wipe away rather
            // than holding a word-sized hole open and snapping shut once the
            // last letter has gone.
            //
            // Mind which edge moves: RXo() measures INWARD FROM THE RIGHT, so
            // a larger design x is further LEFT on screen. The word's visible
            // span runs from design lx + tw_px*(1-reveal) (its screen-right
            // end, the end that empties first) to lx + tw_px (screen-left,
            // fixed). So gap_a - the screen-right edge - is the one that
            // sweeps; moving gap_b instead filled the rail in from the left
            // while the letters vacated from the right, which is backwards.
            // Both margins scale with the reveal too, so at reveal 0 the gap
            // is zero-width and the two segments meet exactly where the
            // unbroken rule in the else branch runs - no step either way.
            float gap_a = lx + tw_px * (1.0f - standby_reveal) - 8.0f * standby_reveal;
            float gap_b = lx + tw_px + 10.0f * standby_reveal;
            rule(false, -kBleed, gap_a, y, 2.2f * U, kRailAlpha);
            if (gap_b < kFrameEnd)
                rule(false, gap_b, kFrameEnd, y, 2.2f * U, kRailAlpha);
            float half_px = (T_STATE / U) * 0.5f;
            canvas_->text_wipe(tag, RXo(lx), MY(456.0f + half_px), T_STATE, true,
                           kRestR, kRestG, kRestB, standby_reveal);
        } else {
            // Same boundary every other rail on this instrument stops at -
            // was 200, short of where the arrow's angled edge begins.
            rule(false, -kBleed, kFrameEnd, y, 2.2f * U, kRailAlpha);
        }
    }
    // Same boundary the top rail stops at - was 224, short of the L252 572
    // vertex where the bottom angled edge begins (mirrored on this panel).
    rule(false, -kBleed, kFrameEnd, MY(596.0f), 2.2f * U, kRailAlpha);

    // Headline: distance in the small slot, ground speed in the big one —
    // mirroring voltage and current on the left.
    {
        float x    = RXo(kEdge);
        float base = MY(512.0f) + 0.25f * T_BIG;   // one baseline for the row
        if (in.bf.have_speed) {
            canvas_->text("km/h", x, BL(base, T_SMALL), T_SMALL, true, lur, lug, lub);
            x -= canvas_->text_width("km/h", T_SMALL);
            snprintf(buf, sizeof(buf), "%.0f", in.bf.speed_kmh);
            canvas_->text(buf, x, BL(base, T_BIG), T_BIG, true,
                      kWhiteR * dim, kWhiteG * dim, kWhiteB * dim);
            x -= canvas_->text_width(buf, T_BIG) + 10.0f * U;
        } else {
            canvas_->text("--", x, BL(base, T_BIG), T_BIG, true, 0.45f, 0.45f, 0.45f);
            x -= canvas_->text_width("--", T_BIG) + 10.0f * U;
        }
        if (in.dist_m >= 0) {
            canvas_->text("\xE2\x80\xA2", x, BL(base, T_UNIT), T_UNIT, true,
                      ldim * 0.42f, ldim * 0.42f, ldim * 0.42f);
            x -= canvas_->text_width("\xE2\x80\xA2", T_UNIT) + 10.0f * U;
            canvas_->text("m", x, BL(base, T_SMALL), T_SMALL, true, lur, lug, lub);
            x -= canvas_->text_width("m", T_SMALL);
            snprintf(buf, sizeof(buf), "%d", in.dist_m);
            canvas_->text(buf, x, BL(base, T_UNIT), T_UNIT, true,
                      ldim * 0.82f, ldim * 0.82f, ldim * 0.82f);
            x -= canvas_->text_width(buf, T_UNIT) + 10.0f * U;
        }
        // The video link's SNR beside the distance: how far, and how well it
        // hears at that distance.
        if (in.snr_db >= 0.0f) {
            canvas_->text("\xE2\x80\xA2", x, BL(base, T_UNIT), T_UNIT, true,
                      ldim * 0.42f, ldim * 0.42f, ldim * 0.42f);
            x -= canvas_->text_width("\xE2\x80\xA2", T_UNIT) + 10.0f * U;
            canvas_->text("dB", x, BL(base, T_SMALL), T_SMALL, true, lur, lug, lub);
            x -= canvas_->text_width("dB", T_SMALL);
            snprintf(buf, sizeof(buf), "%.0f", in.snr_db);
            canvas_->text(buf, x, BL(base, T_UNIT), T_UNIT, true,
                      ldim * 0.82f, ldim * 0.82f, ldim * 0.82f);
        }
    }

    // Track 1 — the control link, from Betaflight's Link Quality element.
    // Track 2 — the video link's MCS rung. They are different systems
    // that fail independently, which is why both are named.
    {
        // Link Quality only. RSSI (a percentage, or CRSF's dBm) is a signal
        // strength, not how much of the control stream is getting through,
        // and mixing the two made the same track mean different things on
        // different receivers. No LQ element, empty track.
        float rc = 0.0f;
        bool have_rc = false;
        if (in.bf.have_lq) { rc = in.bf.lq_pct / 100.0f; have_rc = true; }
        canvas_->text("RC", RXo(kEdge), MY(527.0f), T_LABEL, true,
                  kWhiteR * ldim, kWhiteG * ldim, kWhiteB * ldim);
        float rlab_w = fmaxf(canvas_->text_width("RC", T_LABEL), canvas_->text_width("VIDEO", T_LABEL));
        float rx0    = RXo(271.0f);
        float rtw    = (RXo(kEdge) - rlab_w - kLabelGap * UX) - rx0;
        canvas_->seg_track(rx0, MY(525.0f), rtw, 9.0f * U, kSegsMain,
                       have_rc ? rc : 0.0f, false, kCyanR, kCyanG, kCyanB,
                       0.70f * ldim, 1.00f * ldim, f_block);
        // Amber, then red, with the screen-edge warning (osd.cpp, off these blocks):
        // the track that explains the glow wears its colour.
        float vr = kLimeR, vg = kLimeG, vb = kLimeB;
        if (in.link_warn >= 2)      { vr = 1.0f; vg = 0.30f; vb = 0.25f; }
        else if (in.link_warn == 1) { vr = 1.0f; vg = 0.72f; vb = 0.15f; }
        canvas_->text("VIDEO", RXo(kEdge), MY(537.0f), T_LABEL, true,
                  vr * ldim, vg * ldim, vb * ldim);
        canvas_->seg_track(rx0, MY(534.0f), rtw, 4.0f * U, kSegsThin,
                       in.range_norm, false, vr, vg, vb,
                       0.55f * ldim, 0.85f * ldim, f_block);
    }

    // The two bottom rows are the same idea as the left panel's, mirrored:
    // whose reading it is, said by the icon, at the outboard end where the eye
    // arrives. Air unit first, this box second - so on both panels the drone's
    // row sits above the goggle's.
    // Same metrics as the left panel; only the dimming differs, because this
    // side follows the radio and that one follows the flight controller.
    const float lir = kWhiteR * ldim, lig = kWhiteG * ldim, lib = kWhiteB * ldim;
    const float ltr = lir, ltg = lig, ltb = lib;   // see the left panel

    // Drone row: the link frequency, what the air unit is spending, and how
    // hot that makes it - one "VTX" line rather than three separate rows,
    // since all three are read together and none of them alone is the point.
    {
        float base = MY(560.0f) + 0.25f * T_ROW;
        float x    = RXo(kEdge) - iw;
        icon(VecIcon::Drone, x, base, lir, lig, lib);
        x -= igap;
        char freq[16] = {0}, pw[16] = {0}, tp[16] = {0};
        // The channel and how wide the video link is on it ("5839@20MHz"):
        // the width moves at run time (the radio's own steps, 40 MHz).
        if (in.link_freq_mhz > 0 && in.link_bw_idx >= 0 && in.link_bw_idx < 6)
            snprintf(freq, sizeof(freq), "%d@%sMHz", in.link_freq_mhz, gnd::bw_step_label(in.link_bw_idx));
        else if (in.link_freq_mhz > 0)
            snprintf(freq, sizeof(freq), "%dMHz", in.link_freq_mhz);
        if (in.vtx_pwr[0])        snprintf(pw,   sizeof(pw),   "%s", in.vtx_pwr);
        if (in.vtx_temp_valid)    snprintf(tp,   sizeof(tp),   "%.0fC", in.vtx_temp_c);

        // Frequency and power share one colour; join whichever of the two
        // are present behind a single "VTX" label.
        char lead[48] = {0};
        if (freq[0] && pw[0]) snprintf(lead, sizeof(lead), "VTX %s \xE2\x80\xA2 %s", freq, pw);
        else if (freq[0])     snprintf(lead, sizeof(lead), "VTX %s", freq);
        else if (pw[0])       snprintf(lead, sizeof(lead), "VTX %s", pw);

        float vtxr = ltr, vtxg = ltg, vtxb = ltb;
        if (in.vtx_temp_valid) temp_rgb(in.vtx_temp_c, ltr, ltg, ltb, vtxr, vtxg, vtxb);
        // Right-aligned, so the temperature - the segment that needs its own
        // colour - is drawn last and does not have to be cut out of a string
        // already coloured for the rest of the line.
        if (lead[0] && tp[0]) {
            const float by = BL(base, T_ROW);
            canvas_->text(tp, x, by, T_ROW, true, vtxr, vtxg, vtxb);
            char with_sep[56];
            snprintf(with_sep, sizeof(with_sep), "%s \xE2\x80\xA2 ", lead);
            canvas_->text(with_sep, x - canvas_->text_width(tp, T_ROW), by, T_ROW, true, ltr, ltg, ltb);
        } else if (lead[0]) {
            canvas_->text(lead, x, BL(base, T_ROW), T_ROW, true, ltr, ltg, ltb);
        } else if (tp[0]) {
            char solo[24];
            snprintf(solo, sizeof(solo), "VTX %s", tp);
            canvas_->text(solo, x, BL(base, T_ROW), T_ROW, true, vtxr, vtxg, vtxb);
        }
    }

    // Goggle row: what this box is actually receiving and decoding. Mode,
    // bitrate and latency sit together because each is read against the other
    // two - 18 Mbps is generous at 1080p60 and thin at 1080p120, and neither
    // figure says anything about how long the picture takes to arrive.
    {
        // The measured framerate snapped to a camera's (canopy::snapped_fps).
        const int fps = canopy::snapped_fps(in.fps);
        char mode[24] = "--", rate[24] = "--", lat[24] = "--";
        if (in.v_h > 0 && fps > 0)    snprintf(mode, sizeof(mode), "%dp%d", in.v_h, fps);
        else if (in.v_h > 0)          snprintf(mode, sizeof(mode), "%dp", in.v_h);
        // What the video spends against what the link carries now
        // ("11.2/44.9Mbps"): the headroom, which moves with the MCS and the
        // step to 40 MHz.
        if (in.mbps > 0.05f && in.link_mbps > 0.05f)
            snprintf(rate, sizeof(rate), "%.1f/%.1fMbps", in.mbps, in.link_mbps);
        else if (in.mbps > 0.05f)     snprintf(rate, sizeof(rate), "%.1fMbps", in.mbps);
        if (in.have_lat)              snprintf(lat, sizeof(lat), "%.0fms", in.lat_med_ms);
        // Each slice's own latency, top first, in every frame mode.
        if (in.lat_nslices >= 2) {
            // Each slice, top to bottom: 23|27ms, 22|24|26ms.
            int o = 0;
            for (int s = 0; s < in.lat_nslices && o < (int)sizeof(lat) - 8; s++)
                o += snprintf(lat + o, sizeof(lat) - o, s ? "|%.0f" : "%.0f", in.lat_slice_ms[s]);
            snprintf(lat + o, sizeof(lat) - o, "ms");
        }
        // Rate, latency, then mode - not mode first. The row is right-aligned,
        // so whatever sits at the right end is the one thing that never moves;
        // giving that slot to the video mode (which changes only when the
        // camera does) stops the label sliding left and right every time the
        // bitrate or latency figure gains or loses a digit.
        snprintf(buf, sizeof(buf), "%s \xE2\x80\xA2 %s \xE2\x80\xA2 %s", rate, lat, mode);

        float base = MY(584.0f) + 0.25f * T_ROW;
        float x    = RXo(kEdge) - iw;
        icon(VecIcon::Goggles, x, base, lir, lig, lib);
        x -= igap;
        canvas_->text(buf, x, BL(base, T_ROW), T_ROW, true, ltr, ltg, ltb);
    }

    // HUD > Debug Info: one line under the panel's bottom rail (596), lined
    // up with the rows' text.
    if (in.debug[0]) {
        const float base = MY(612.0f) + 0.25f * T_LABEL;
        canvas_->text(in.debug, RXo(kEdge) - iw - igap, BL(base, T_LABEL), T_LABEL, true, ltr, ltg, ltb);
    }
}

// While a side is being picked: each blade's name over it, the picked one in
// the accent, and under it the sections it holds. Set in the blade's own
// space, so the names take its slant and its depth like everything on it; the
// blade's lift is in the panel matrices already. pick: their opacity.
void OSD::draw_canopy_pick(const CanopyIn& in, float pick) {
    const HudTheme& TH = hud_theme_current();
    const CanopyFrame F = canopy_frame(in.frustum_w, in.frustum_h, in.overscan, in.s, wrap_depth);
    const float T_NAME = 0.125f * F.sd;   // the size of the blades' state word (T_STATE)
    const float T_LIST = 0.082f * F.sd;   // T_UNIT
    // Above the top rail (456), and clear of the REC marker sitting on it (431-442).
    const float name_base = F.MY(407.0f), list_base = F.MY(427.0f);
    auto BL = [](float baseline, float size) { return baseline - 0.25f * size; };
    for (int side = 0; side < 2; side++) {
        const bool left = (side == 0);
        math::Mat4 mvp;
        slant_mvp(left ? in.model_l : in.model_r, in.vp, left ? F.LXo(56.0f) : F.RXo(56.0f), F.MY(502.0f),
                  left ? kSlantRad : -kSlantRad, mvp);
        canvas_->set_transform(mvp);
        const bool on = (ui_side_ == side);
        const float* nc = on ? TH.accent : TH.quiet;
        const float* lc = on ? TH.text : TH.quiet;
        const float x = left ? F.LXo(kEdge) : F.RXo(kEdge);
        canvas_->set_fade(pick);
        canvas_->text(left ? "<  GROUND UNIT" : "AIR UNIT  >", x, BL(name_base, T_NAME), T_NAME, !left,
                  nc[0], nc[1], nc[2]);
        canvas_->set_fade(pick * (on ? 0.85f : 0.6f));
        canvas_->text(left ? "HUD   DISPLAY   DVR   SYSTEM" : "VIDEO   RADIO   SERIAL   INFO", x, BL(list_base, T_LIST), T_LIST, !left,
                  lc[0], lc[1], lc[2]);
    }
    canvas_->set_fade(1.0f);
}

void OSD::draw_canopy(Frame& f) {
    auto& tmp = f.tmp;
    auto& model_l = f.model_l;
    auto& model_r = f.model_r;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& latency_median_ms = f.latency_median_ms;
    auto& have_latency_median = f.have_latency_median;
    auto& latency_slice_ms = f.latency_slice_ms;
    auto& latency_nslices = f.latency_nslices;
    auto& v_width = f.v_width;
    auto& v_height = f.v_height;
    auto& cur_fps = f.cur_fps;
    auto& v_bw = f.v_bw;
    auto& s = f.s;
    auto& canopy_on = f.canopy_on;
    auto& canopy = f.canopy;
    auto& video_active = f.video_active;
    if (canopy_on) {
        canopy.frustum_w = frustum_w;
        canopy.frustum_h = frustum_h;
        canopy.overscan  = 0.02f;
        canopy.s         = s;
        canopy.model_l   = model_l;
        canopy.model_r   = model_r;
        canopy.vp        = tmp;
        canopy.video_active   = video_active;
        canopy.vtx_temp_valid = vtx_temp_valid;
        canopy.vtx_temp_c     = vtx_cpu_c;
        canopy.vtx_low_power  = vtx_low_power;
        canopy.vrx_temp_c     = vrx_temp_cached_;
        canopy.vrx_volts      = vrx_volts_cached_;
        {
            const uint64_t t = get_time_us();
            if (t - hud_wifi_us_ > 1000000) { hud_wifi_us_ = t; hud_wifi_on_ = wifi_ap_on(); }
            canopy.wifi_on = hud_wifi_on_;
        }
        canopy.bf             = msp_osd.get_telem();
        canopy.dist_m = dist_shown();
        canopy.snr_db = osd_vars.radio_link.state == 2 ? std::max(0.0f, osd_vars.radio_link.snr) : -1.0f;
        canopy.v_w  = (int)v_width;
        canopy.v_h  = (int)v_height;
        canopy.fps  = cur_fps;
        canopy.mbps = video_active ? v_bw : 0.0f;
        // Median, not mean: one stalled frame drags an average somewhere the
        // link never actually was, and the figure is there to say what the
        // link normally feels like.
        if (video_active && have_latency_median) {
            canopy.lat_med_ms = latency_median_ms;
            // One per slice in every frame mode: a picture put up whole still
            // has its lower slice lit later, mid-screen.
            canopy.lat_nslices = latency_nslices;
            std::copy(latency_slice_ms, latency_slice_ms + kLatSlices, canopy.lat_slice_ms);
            canopy.have_lat = true;
        }
        prof::mark(prof::kDrawPre);
        draw_canopy_hud(canopy);
        prof::mark(prof::kCanopy);
    }
}

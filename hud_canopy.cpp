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

#include "osd.hpp"
#include "settings.hpp"
#include "vec_icons.hpp"
#include "hud_theme.hpp"
#include "utils/time_util.h"
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

// Space between a track's name and the track itself, in mockup pixels. This
// is the number the layout actually cares about; where the track starts falls
// out of it once the label has been measured.
constexpr float kLabelGap = 12.0f;

// How far past the screen edge the frame is drawn. The dynamic HUD translates
// the panels with the aircraft's accelerometer - about 0.025 frustum units per
// G, with spring overshoot on top - so a rail that begins exactly at the edge
// swings its own end into view on a hard manoeuvre. Everything anchored to the
// edge starts out here instead, comfortably past anything the spring reaches.
constexpr float kBleed = 60.0f;

// Rest alpha for every rail line, all four of them, both panels. Was 0.95 -
// close enough to solid that the rails read brighter than the rest of the
// instrument at a glance.
constexpr float kRailAlpha = 0.8f;

// How far in from the screen edge the content starts. Was 28, which the mockup
// inherited from a 1200px canvas with wider margins all round; on the goggle it
// was giving away more of the frame than the layout needs.
constexpr float kEdge = 14.0f;

// In-plane slant. Everything in a panel - the wedge, the rules, the tracks and
// every line of type - shares this one angle, rising towards the centre of the
// screen. The mockup gave the frame and the content slightly different slopes
// (1.8 degrees on the wedge's top edge against 3.4 on the text), which reads
// as a mistake rather than as depth: a blade is one object, and the moment two
// of its edges disagree the eye stops seeing it as one.
constexpr float kSlantRad = 0.05934f;   // 3.4 degrees

// Leaving CONNECTED: hold and mark. The panel drops to half quickly, then the
// rails pulse once and IDLE strikes in behind the pulse - the point being to
// mark the moment the numbers stopped being true, rather than to let them fade
// out and hope it is noticed. Losing the link is worth a beat of attention; the
// readings on screen are about to become history without changing value.
constexpr float kIdleBriUs   = 240000.0f;   // brightness falls to half
constexpr float kIdlePulseUs = 360000.0f;   // ...the rails pulse once
constexpr float kIdleMarkAt  = 440000.0f;   // ...IDLE strikes in behind it
constexpr float kIdleMarkUs  = 240000.0f;
constexpr float kIdleAllUs   = 800000.0f;
// Where the dimmed panel settles. Named because three separate things have to
// agree on it: the content, the rails, and the floor the pulse returns to.
constexpr float kIdleFloor   = 0.5f;

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

// Link acquired. Not ambient motion - it marks the moment the radio finds the
// aircraft, which is a moment you are already watching for. The phases run
// outboard to inboard, the direction the perspective recedes, so the panel
// reads as arriving from off-screen rather than fading up where it already is.
constexpr float kLinkRuleUs  = 120000.0f;   // rules run in from the edges
constexpr float kLinkShadeUs = 260000.0f;   // ...then the ground fades up
constexpr float kLinkAllUs   = 420000.0f;   // ...then the blocks sweep in

// Segment counts: the coarse "state" track and the fine "stress" one.
constexpr int kSegsMain = 20;
constexpr int kSegsThin = 7;      // the MCS ladder's seven rungs

}  // namespace

// Edge feathering: a cheap stand-in for multisampling. The canopy's shapes
// are drawn flat and then slanted and put in perspective by the panel
// matrices, so their edges land on the screen as diagonals with a hard
// staircase. Instead of 4x MSAA (four times the blending work on this
// one-core GPU - see drm.cpp), each rail, block and wedge edge gets a border
// about a pixel wide whose alpha runs from the shape's down to zero, and the
// ordinary blend smooths the step. aa_feather_ is that width in canopy units
// (0 = off; set per frame in draw_canopy_hud from hud_feather).
namespace {
// Quad a-b-c-d (in loop order) as two triangles, each corner with its alpha.
void quad6(std::vector<float>& v, float ax, float ay, float aa, float bx, float by, float ba,
           float cx, float cy, float ca, float dx, float dy, float da) {
    v.insert(v.end(), {ax, ay, 0.0f, 0.5f, 0.5f, aa, bx, by, 0.0f, 0.5f, 0.5f, ba,
                       cx, cy, 0.0f, 0.5f, 0.5f, ca, ax, ay, 0.0f, 0.5f, 0.5f, aa,
                       cx, cy, 0.0f, 0.5f, 0.5f, ca, dx, dy, 0.0f, 0.5f, 0.5f, da});
}
// A feathered axis-aligned rectangle: the body at alpha a, then a ring f
// wide fading to zero around it.
void feathered_rect(std::vector<float>& v, float x0, float y0, float x1, float y1, float a, float f) {
    quad6(v, x0, y0, a, x1, y0, a, x1, y1, a, x0, y1, a);
    if (f <= 0.0f) return;
    quad6(v, x0 - f, y0 - f, 0, x1 + f, y0 - f, 0, x1, y0, a, x0, y0, a);   // bottom
    quad6(v, x0, y1, a, x1, y1, a, x1 + f, y1 + f, 0, x0 - f, y1 + f, 0);   // top
    quad6(v, x0 - f, y0 - f, 0, x0, y0, a, x0, y1, a, x0 - f, y1 + f, 0);   // left
    quad6(v, x1, y0, a, x1 + f, y0 - f, 0, x1 + f, y1 + f, 0, x1, y1, a);   // right
}
}  // namespace

// A run of tapering blocks. The tall, opaque end sits at the screen edge and
// the run shrinks and fades inward, which is what gives the panel its depth
// without dimming anything (dimming reads as less important, never as further
// away). Segments past `filled` stay drawn, very faint, so the track's length
// is always legible and a falling gauge retreats towards the edge rather than
// disappearing.
void OSD::draw_seg_track(float x, float y, float w, float h, int segs,
                         float filled, bool edge_left,
                         float r, float g, float b, float a_lo, float a_hi,
                         float reveal) {
    if (segs <= 0 || w <= 0.0f || reveal <= 0.0f) return;
    int shown = (int)(reveal * segs + 0.5f);
    if (shown < 1)     shown = 1;
    if (shown > segs)  shown = segs;
    const float taper = 0.32f;
    // Keep the 10:3 block-to-gap ratio whatever the segment count, and solve
    // the pitch so the tapered run ends up exactly `w` wide.
    const float pitch = w / (segs * (1.0f - taper / 2.0f));
    const float seg_w = pitch * 10.0f / 13.0f;
    const float gap_w = pitch *  3.0f / 13.0f;

    int lit = (int)(filled * segs + 0.5f);
    if (lit < 0) lit = 0;
    if (lit > segs) lit = segs;

    std::vector<float> v;
    v.reserve(segs * 6 * 6);
    float cx = edge_left ? x : x + w;
    for (int i = 0; i < shown; i++) {
        float t  = (segs > 1) ? (float)i / (float)(segs - 1) : 0.0f;
        float sc = 1.0f - taper * t;
        float sw = seg_w * sc, sh = h * sc, sg = gap_w * sc;
        float x0 = edge_left ? cx : cx - sw;
        float x1 = x0 + sw;
        float y0 = y + (h - sh) * 0.5f;
        float y1 = y0 + sh;
        cx = edge_left ? (cx + sw + sg) : (cx - sw - sg);

        float a = (i < lit) ? (a_hi - (a_hi - a_lo) * t) : 0.13f;
        feathered_rect(v, x0, y0, x1, y1, a, aa_feather_);
    }

    glUniform1i(u_is_text_, 0);
    glUniform1i(u_use_shading_, 0);
    glUniform4f(u_color_, r, g, b, 1.0f);
    glUniform1f(u_alpha_, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(a_pos_);
    glVertexAttribPointer(a_pos_, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(a_uv_);
    glVertexAttribPointer(a_uv_, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    if (a_alpha_factor_ != -1) {
        glEnableVertexAttribArray(a_alpha_factor_);
        glVertexAttribPointer(a_alpha_factor_, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 6));
    if (a_alpha_factor_ != -1) {
        glDisableVertexAttribArray(a_alpha_factor_);
        glVertexAttrib1f(a_alpha_factor_, 1.0f);   // restore for every other draw
    }
}

// The panel ground. The mockup's outline is a kite: tallest at the screen
// edge, its top and bottom edges converging as it runs inboard, closing to a
// point. Both edges have to slope - drawn with a constant half-height it comes
// out as a translucent slab with two hard horizontal edges, which over a
// textured video frame reads as a grey box laid on the picture rather than as
// a shape belonging to the panel.
//
// Columns are passed in already mapped to frustum space so the mockup's own
// coordinates stay legible at the call site.
void OSD::draw_fade_wedge(const float* xs, const float* tops, const float* bots,
                          const float* alphas, int n, float r, float g, float b) {
    if (n < 2) return;
    std::vector<float> v;
    v.reserve(n * 2 * 6);
    // As triangles now (was a strip), so the feathered edges can join it.
    for (int i = 0; i + 1 < n; i++) {
        quad6(v, xs[i], bots[i], alphas[i], xs[i + 1], bots[i + 1], alphas[i + 1],
                 xs[i + 1], tops[i + 1], alphas[i + 1], xs[i], tops[i], alphas[i]);
        if (aa_feather_ > 0.0f) {
            const float f = aa_feather_;
            quad6(v, xs[i], tops[i], alphas[i], xs[i + 1], tops[i + 1], alphas[i + 1],
                     xs[i + 1], tops[i + 1] + f, 0, xs[i], tops[i] + f, 0);
            quad6(v, xs[i], bots[i] - f, 0, xs[i + 1], bots[i + 1] - f, 0,
                     xs[i + 1], bots[i + 1], alphas[i + 1], xs[i], bots[i], alphas[i]);
        }
    }

    glUniform1i(u_is_text_, 0);
    glUniform1i(u_use_shading_, 0);
    glUniform4f(u_color_, r, g, b, 1.0f);
    glUniform1f(u_alpha_, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(a_pos_);
    glVertexAttribPointer(a_pos_, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(a_uv_);
    glVertexAttribPointer(a_uv_, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    if (a_alpha_factor_ != -1) {
        glEnableVertexAttribArray(a_alpha_factor_);
        glVertexAttribPointer(a_alpha_factor_, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 6));
    if (a_alpha_factor_ != -1) {
        glDisableVertexAttribArray(a_alpha_factor_);
        glVertexAttrib1f(a_alpha_factor_, 1.0f);
    }
}

// A hairline that starts solid at (x0,y0) and fades to nothing at (x1,y1).
void OSD::draw_fade_rule(float x0, float y0, float x1, float y1, float thick,
                         float a0, float r, float g, float b, float a1) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    float ux = -dy / len, uy = dx / len;              // unit normal
    float nx = ux * thick * 0.5f, ny = uy * thick * 0.5f;
    float fx = ux * aa_feather_, fy = uy * aa_feather_;
    // Body, plus a feather strip either side along the rule's length.
    std::vector<float> v;
    v.reserve(18 * 6);
    quad6(v, x0 - nx, y0 - ny, a0, x1 - nx, y1 - ny, a1, x1 + nx, y1 + ny, a1, x0 + nx, y0 + ny, a0);
    if (aa_feather_ > 0.0f) {
        quad6(v, x0 - nx - fx, y0 - ny - fy, 0, x1 - nx - fx, y1 - ny - fy, 0,
                 x1 - nx, y1 - ny, a1, x0 - nx, y0 - ny, a0);
        quad6(v, x0 + nx, y0 + ny, a0, x1 + nx, y1 + ny, a1,
                 x1 + nx + fx, y1 + ny + fy, 0, x0 + nx + fx, y0 + ny + fy, 0);
    }

    glUniform1i(u_is_text_, 0);
    glUniform1i(u_use_shading_, 0);
    glUniform4f(u_color_, r, g, b, 1.0f);
    glUniform1f(u_alpha_, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(a_pos_);
    glVertexAttribPointer(a_pos_, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(a_uv_);
    glVertexAttribPointer(a_uv_, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    if (a_alpha_factor_ != -1) {
        glEnableVertexAttribArray(a_alpha_factor_);
        glVertexAttribPointer(a_alpha_factor_, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 6));
    if (a_alpha_factor_ != -1) {
        glDisableVertexAttribArray(a_alpha_factor_);
        glVertexAttrib1f(a_alpha_factor_, 1.0f);
    }
}

void OSD::draw_canopy_hud(const CanopyIn& in) {
    const HudTheme& TH = hud_theme_current();
    // About 1.2 px (half the frustum height is 540 px at 1080p).
    static const bool feather = Settings::getInstance().getBool("hud_feather", true);
    aa_feather_ = feather ? in.frustum_h / 540.0f * 1.2f : 0.0f;
    const float kWhiteR = TH.text[0],   kWhiteG = TH.text[1],   kWhiteB = TH.text[2];
    const float kCyanR  = TH.data[0],   kCyanG  = TH.data[1],   kCyanB  = TH.data[2];
    const float kLimeR  = TH.accent[0], kLimeG  = TH.accent[1], kLimeB  = TH.accent[2];
    const float kShadeR = TH.ground[0], kShadeG = TH.ground[1], kShadeB = TH.ground[2];
    const float kIdleR  = TH.quiet[0],  kIdleG  = TH.quiet[1],  kIdleB  = TH.quiet[2];

    // Temperature is the one figure with a real physical ceiling, so it gets
    // a colour of its own rather than the theme's - a warning has to read the
    // same on every theme, or it stops being a warning. Amber then red, not
    // themed: this is a safety signal, not a hue that changes with taste.
    constexpr float kTempWarnC = 70.0f, kTempHotC = 85.0f;
    auto temp_rgb = [&](float c, float base_r, float base_g, float base_b,
                        float& r, float& g, float& b) {
        if (c >= kTempHotC)       { r = 1.0f; g = 0.30f; b = 0.25f; }
        else if (c >= kTempWarnC) { r = 1.0f; g = 0.72f; b = 0.15f; }
        else                      { r = base_r; g = base_g; b = base_b; }
    };

    // The goggle's own supply. Per-cell so it reads the same on any pack:
    // the ADC gives a raw pack voltage and the cell count is inferred from it
    // (3S/4S/6S are what this barrel jack sees in practice). Thresholds are
    // the usual lithium ones - 3.5V/cell is "land soon", 3.3V is "now".
    constexpr float kCellWarnV = 3.50f, kCellLowV = 3.30f;
    auto cells_for = [](float v) {
        if (v <= 0.0f)  return 0;
        if (v < 9.0f)   return 2;
        if (v < 13.0f)  return 3;
        if (v < 17.5f)  return 4;
        if (v < 21.5f)  return 5;
        return 6;
    };
    auto volt_rgb = [&](float per_cell, float base_r, float base_g, float base_b,
                        float& r, float& g, float& b) {
        if (per_cell <= kCellLowV)       { r = 1.0f; g = 0.30f; b = 0.25f; }
        else if (per_cell <= kCellWarnV) { r = 1.0f; g = 0.72f; b = 0.15f; }
        else                             { r = base_r; g = base_g; b = base_b; }
    };

    // Panel height in real pixels, so the vector icons can be rasterized at
    // the size they are actually drawn rather than upsampled from a fixed one.
    const int screen_px_h = (dev && dev->output_list)
                          ? (int)dev->output_list->mode.vdisplay : 1080;
    const float s = in.s;

    // Both wings are drawn through a matrix that rotates them 0.26 rad about
    // Y, which swings their outer edge towards the camera - so a vertex at the
    // frustum's half-width does NOT project to the screen edge, it projects
    // well past it. Solve for the model-space x that lands on the edge:
    //
    //   project(x) = x*cos0 * D / (D - x*sin0) = target
    //   =>      x  = target*D / (cos0*D + target*sin0)
    //
    // and take the magnification there, which every other dimension is divided
    // by so the panel is the size it was designed at once projected. The
    // magnification falls off inboard, which is the depth the layout wants:
    // elements shrink as they approach the centre rather than being dimmed.
    const float kTilt = 0.26f;
    const float ct = cosf(kTilt), st = sinf(kTilt);
    const float D  = wrap_depth;
    const float target_x = in.frustum_w - in.overscan;
    const float ex = target_x * D / (ct * D + target_x * st);
    const float mag = D / (D - st * ex);

    // One mockup pixel, mapped through the height so the panels keep the
    // design's proportions; a wider screen widens the gap between them, which
    // is the whole point of anchoring to the edges.
    //
    // The two axes are mapped differently, and deliberately.
    //
    // Vertically, one mockup pixel is 1/620 of the frame height and takes the
    // UI scale: bigger type needs proportionally more room between rows.
    //
    // Horizontally, one mockup pixel is 1/1200 of the frame WIDTH and ignores
    // the UI scale entirely. Two reasons. Mapping x through the height would
    // tie the composition to the aspect ratio, so the same layout would claim
    // more of a 16:9 frame than of the 1200x620 it was drawn on. And scaling x
    // with the UI setting means asking for legible type also buys a wider
    // panel - at 1.3x that pushed each wing from 23% of the width to 34%, so
    // the two of them ate more of the frame than the video kept. Turning the
    // scale up is a request for bigger text, not for a bigger HUD; the centre
    // of the screen is the thing the whole layout exists to protect.
    // The UI scale is damped rather than applied outright. The canopy is drawn
    // at a fixed fraction of the frame, so a scale that grows type by a third
    // grows nothing to put it in - the headline simply runs off the end of its
    // own track. A third of the setting keeps the panel recognisably the shape
    // it was designed as while still answering "make it bigger".
    const float sd = 1.0f + (s - 1.0f) * 0.35f;
    const float base = (in.frustum_h * 2.0f) / 620.0f;
    const float UX = ((in.frustum_w * 2.0f) / 1200.0f) / mag;  // columns
    const float U  = (base * sd) / mag;                         // rows
    const float LX = -ex;                                   // left screen edge
    const float RX =  ex;                                   // right screen edge
    const float BY = (-in.frustum_h + in.overscan) / mag;    // bottom

    // Mockup y (from the top of its 620px frame) -> frustum y.
    // The panels sat 24 design px clear of the bottom edge. Half that reads
    // better - they are anchored to the frame, and a wide margin under them
    // makes them look like they are floating rather than sitting on it.
    constexpr float kDrop = 12.0f;
    auto MY = [&](float y) { return BY + (620.0f - kDrop - y) * U; };

    // The highest thing the blades draw. Published because the menu has to
    // sit clear of it, and it moves: the panels are laid out in mockup pixels
    // scaled by the UI setting, so a bigger HUD pushes this up the screen.
    // A menu with a constant margin baked in is only ever right at 1.0x.
    canopy_top_y_ = MY(456.0f);
    auto LXo = [&](float x) { return LX + x * UX; };    // inward from the left
    auto RXo = [&](float x) { return RX - x * UX; };    // inward from the right

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
    if (have_aircraft != canopy_link_prev_)
        (have_aircraft ? canopy_link_up_ : canopy_link_down_) = now_us;
    canopy_link_prev_ = have_aircraft;
    if (!canopy_link_up_) return;            // IDLE_1

    // Leaving CONNECTED, in three overlapping beats. Everything below reads
    // these rather than branching on have_aircraft, so the settled idle panel
    // and the last frame of the transition are the same drawing.
    //
    //   idle_dim   content and ground brightness, 1 -> kIdleFloor
    //   rail_dim   the same, but with the pulse riding over it
    //   mark       IDLE fading in and the rail gap opening under it
    float idle_dim = 1.0f, rail_dim = 1.0f, mark = 0.0f;
    if (!have_aircraft) {
        idle_dim = rail_dim = kIdleFloor;
        mark = 1.0f;
        if (canopy_link_down_) {
            const float e = (float)(now_us - canopy_link_down_);
            if (e < kIdleAllUs) {
                const float bri = fminf(1.0f, e / kIdleBriUs);
                idle_dim = 1.0f - (1.0f - kIdleFloor) * bri;
                rail_dim = idle_dim;
                // One pulse, peaking mid-way and returning to the floor it
                // started from - a beat, not a flash that leaves the rails
                // brighter than they were.
                const float pt = (e - kIdleBriUs) / kIdlePulseUs;
                if (pt > 0.0f && pt < 1.0f)
                    rail_dim = kIdleFloor + (1.0f - kIdleFloor) * sinf(pt * (float)M_PI);
                mark = fmaxf(0.0f, fminf(1.0f, (e - kIdleMarkAt) / kIdleMarkUs));
                // Nothing else need be driving repaints once the link is gone.
                signal_render(prof::kWakeAnim);
            }
        }
    } else {
        canopy_link_down_ = 0;
    }

    // The rebuild: rails run in from the edge, the ground fades up behind them,
    // then the blocks arrive. Past kLinkAllUs every factor is 1 and this costs
    // nothing, which is the state IDLE_2 sits in - dimmed by idle_dim, not by
    // these.
    float f_rule = 1.0f, f_shade = 1.0f, f_block = 1.0f;
    {
        float e = (float)(now_us - canopy_link_up_);
        if (e < kLinkAllUs) {
            f_rule  = fminf(1.0f, e / kLinkRuleUs);
            f_shade = fmaxf(0.0f, fminf(1.0f, (e - kLinkRuleUs) / (kLinkShadeUs - kLinkRuleUs)));
            f_block = fmaxf(0.0f, fminf(1.0f, (e - kLinkShadeUs) / (kLinkAllUs - kLinkShadeUs)));
            signal_render(prof::kWakeAnim);
        }
    }


    // Text sizes, in the same units draw_text() takes elsewhere in the HUD.
    const float T_BIG   = 0.185f * sd;   // 46px headline
    const float T_UNIT  = 0.082f * sd;   // 19-20px unit / secondary figure
    const float T_SMALL = 0.055f * sd;   // 13px unit suffix
    const float T_ROW   = 0.058f * sd;   // 13px detail rows
    const float T_LABEL = 0.040f * sd;   // 9px track names
    // The flight timer and the ARMED/DISARMED word share the top rail's gap.
    // Sized between the headline voltage and the current beside it: the timer
    // is one of the readings, not a caption on the frame, and at anything
    // smaller it was not legible in flight.
    const float T_STATE = 0.125f * sd;   // between T_BIG (0.185) and T_UNIT (0.082)

    // draw_text() places the BOTTOM of the glyph box at y, and the cairo
    // surface behind it draws the baseline at row 48 of 64 - a quarter of the
    // way up. So two sizes drawn at one y do NOT share a baseline: the smaller
    // sits low by a quarter of the difference, which is what made the current
    // read as a separate line from the voltage rather than part of it.
    // BL() converts a baseline to the y draw_text wants, for any size.
    auto BL = [](float baseline, float size) { return baseline - 0.25f * size; };

    // ...and the same problem bit the icons. draw_icon() takes the bottom of
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
        if (n >= sizeof(ref)) return text_width(sN, size);
        for (size_t i = 0; i < n; i++)
            ref[i] = isdigit((unsigned char)sN[i]) ? '0' : sN[i];
        ref[n] = 0;
        return text_width(ref, size);
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
            draw_fade_rule(X(s), y, X(e), y, th, A(s), c[0], c[1], c[2], A(e));
        }
    };

    // The slant, as a matrix rather than a transform threaded through every
    // draw call: rotate in the panel's own plane about a pivot on its outer
    // edge, then hand the result to the existing model/view/projection chain.
    // Everything drawn under it - wedge, rules, blocks, glyph quads - takes the
    // same angle by construction, which is the only way the frame and the
    // content cannot disagree.
    auto slanted = [&](const float* model, float pivot_x, float pivot_y,
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
        math::multiply(out, m, in.vp);
    };
    math::Mat4 mvp_ls, mvp_rs;
    slanted(in.model_l, LXo(56.0f), MY(502.0f),  kSlantRad, mvp_ls);
    slanted(in.model_r, RXo(56.0f), MY(502.0f), -kSlantRad, mvp_rs);

    // ---------------- LEFT PANEL — the aircraft ----------------
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp_ls);

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
    static const float kWedgeX[]   = { -kBleed, 156.0f, 252.0f, 300.0f };
    // The last row's baseline is 584, so a floor at 612 left 28px of empty
    // ground under it against 4px above the headline. Brought up to match, and
    // the tip's apex follows the new mid-line.
    static const float kWedgeTop[] = { 462.0f, 462.0f, 462.0f, 526.0f };
    static const float kWedgeBot[] = { 590.0f, 590.0f, 590.0f, 526.0f };
    static const float kWedgeA[]   = {  0.66f,  0.34f,  0.11f,   0.0f };
    {
        float xs[4], tops[4], bots[4];
        for (int i = 0; i < 4; i++) {
            xs[i]   = LXo(kWedgeX[i]);
            tops[i] = MY(kWedgeTop[i]);
            bots[i] = MY(kWedgeBot[i]);
        }
        float wa[4];
        for (int i = 0; i < 4; i++) wa[i] = kWedgeA[i] * f_shade * idle_dim;
        draw_fade_wedge(xs, tops, bots, wa, 4, kShadeR, kShadeG, kShadeB);
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
        draw_poly({{x, y}, {x + d, y}, {x + d, y + d}, {x, y + d}},
                  1.0f, 0.95f, 0.15f, 0.15f);
        snprintf(buf, sizeof(buf), "REC %u:%02u", in.rec_secs / 60, in.rec_secs % 60);
        draw_text(buf, x + d + 8.0f * U, MY(442.0f), T_ROW, false, 1.0f, 0.35f, 0.35f);
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
            draw_text(buf, x, BL(base, T_BIG), T_BIG, false,
                      kWhiteR * dim, kWhiteG * dim, kWhiteB * dim);
            x += num_adv(buf, T_BIG);
            draw_text("V", x, BL(base, T_UNIT), T_UNIT, false, ur, ug, ub);
            x += text_width("V", T_UNIT) + 10.0f * U;
        } else {
            draw_text("--", x, BL(base, T_BIG), T_BIG, false, 0.45f, 0.45f, 0.45f);
            x += text_width("--", T_BIG) + 10.0f * U;
        }
        if (in.bf.have_amps) {
            draw_text("\xE2\x80\xA2", x, BL(base, T_UNIT), T_UNIT, false,
                      dim * 0.42f, dim * 0.42f, dim * 0.42f);
            x += text_width("\xE2\x80\xA2", T_UNIT) + 10.0f * U;
            state_x = x;                            // the state label lines up here
            snprintf(buf, sizeof(buf), "%.1f", in.bf.amps);
            draw_text(buf, x, BL(base, T_UNIT), T_UNIT, false,
                      dim * 0.82f, dim * 0.82f, dim * 0.82f);
            x += num_adv(buf, T_UNIT);
            draw_text("A", x, BL(base, T_SMALL), T_SMALL, false, ur, ug, ub);
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
            float tw_px = text_width(tag, T_STATE) / UX;
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
            draw_text(tag, LXo(lx), MY(456.0f + half_px), T_STATE, false, lc[0], lc[1], lc[2]);
        } else {
            // No label to interrupt it: keep it short rather than drawing one
            // long unbroken bar across the top of the video.
            rule(true, -kBleed, 150.0f, y, 2.2f * U, kRailAlpha);
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
        draw_text("CELL", LXo(kEdge), MY(527.0f), T_LABEL, false,
                  kWhiteR * dim, kWhiteG * dim, kWhiteB * dim);
        // The track starts a fixed gap after the widest of the two names, not
        // at a hard-coded inset. The inset was a guess at how wide the labels
        // would render, and this font is condensed and unspaced where the
        // design's was not - so the guess left a third of the panel empty
        // between a name and the thing it names. Measure instead.
        float lab_w = fmaxf(text_width("CELL", T_LABEL), text_width("SAG", T_LABEL));
        float tx0   = LXo(kEdge) + lab_w + kLabelGap * UX;
        float tw    = LXo(271.0f) - tx0;
        draw_seg_track(tx0, MY(525.0f), tw, 9.0f * U, kSegsMain,
                       lvl, true, kCyanR, kCyanG, kCyanB, 0.70f * dim, 1.00f * dim,
                       f_block);
        draw_text("SAG", LXo(kEdge), MY(537.0f), T_LABEL, false,
                  kLimeR * dim, kLimeG * dim, kLimeB * dim);
        draw_seg_track(tx0, MY(534.0f), tw, 4.0f * U, kSegsMain,
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
        GLuint t = vec_icon_texture(which, ipx, &asp);
        if (!t) return;
        float h = ih, w = ih * asp;
        if (w > iw) { w = iw; h = iw / asp; }
        draw_icon(t, x + (iw - w) * 0.5f, IY(base, T_ROW, h), w, h, r, g, b);
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
            draw_text(in.bf.mode, x, BL(base, T_ROW), T_ROW, false, tr, tg, tb);
            x += text_width(in.bf.mode, T_ROW) + igap;
        }
        if (in.dist_m >= 0) {
            draw_text("\xE2\x80\xA2", x, BL(base, T_ROW), T_ROW, false,
                      tr * 0.68f, tg * 0.68f, tb * 0.68f);
            x += text_width("\xE2\x80\xA2", T_ROW) + igap;
            icon(VecIcon::Distance, x, base, ir, ig, ib);
            x += iw + igap;
            if (in.dist_m >= 1000) snprintf(buf, sizeof(buf), "%.2fkm", in.dist_m / 1000.0f);
            else                   snprintf(buf, sizeof(buf), "%dm", in.dist_m);
            draw_text(buf, x, BL(base, T_ROW), T_ROW, false, tr, tg, tb);
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
        draw_text(buf, x, BL(base, T_ROW), T_ROW, false, vr, vg, vb);

        // The goggle's own supply, beside its temperature - the two readings
        // that are about this unit rather than the aircraft. Drawn only when
        // the ADC has actually produced a value; an unwired or failed read
        // leaves the slot empty instead of showing a plausible-looking zero.
        if (in.vrx_volts > 0.5f) {
            x += text_width(buf, T_ROW) + igap * 1.6f;
            const int   n  = cells_for(in.vrx_volts);
            const float pc = n ? in.vrx_volts / (float)n : 0.0f;
            float br, bg, bb;
            volt_rgb(pc, tr, tg, tb, br, bg, bb);
            snprintf(buf, sizeof(buf), "%.1fV", in.vrx_volts);
            draw_text(buf, x, BL(base, T_ROW), T_ROW, false, br, bg, bb);
        }
    }

    // ---------------- RIGHT PANEL — the links ----------------
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp_rs);

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
        draw_fade_wedge(xs, tops, bots, wa, 4, kShadeR, kShadeG, kShadeB);
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
    const float sb_disarmed_end = (kEdge + 46.0f) + text_width("DISARMED", T_STATE) / UX;
    const float sb_tw_px = text_width(kStandbyTag, T_STATE) / UX;
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
            float tw_px = text_width(tag, T_STATE) / UX;
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
            draw_text(tag, RXo(lx), MY(456.0f + half_px), T_STATE, true,
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
            draw_text_wipe(tag, RXo(lx), MY(456.0f + half_px), T_STATE, true,
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
            draw_text("km/h", x, BL(base, T_SMALL), T_SMALL, true, lur, lug, lub);
            x -= text_width("km/h", T_SMALL);
            snprintf(buf, sizeof(buf), "%.0f", in.bf.speed_kmh);
            draw_text(buf, x, BL(base, T_BIG), T_BIG, true,
                      kWhiteR * dim, kWhiteG * dim, kWhiteB * dim);
            x -= text_width(buf, T_BIG) + 10.0f * U;
        } else {
            draw_text("--", x, BL(base, T_BIG), T_BIG, true, 0.45f, 0.45f, 0.45f);
            x -= text_width("--", T_BIG) + 10.0f * U;
        }
        if (in.dist_m >= 0) {
            draw_text("\xE2\x80\xA2", x, BL(base, T_UNIT), T_UNIT, true,
                      ldim * 0.42f, ldim * 0.42f, ldim * 0.42f);
            x -= text_width("\xE2\x80\xA2", T_UNIT) + 10.0f * U;
            draw_text("m", x, BL(base, T_SMALL), T_SMALL, true, lur, lug, lub);
            x -= text_width("m", T_SMALL);
            snprintf(buf, sizeof(buf), "%d", in.dist_m);
            draw_text(buf, x, BL(base, T_UNIT), T_UNIT, true,
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
        draw_text("RC", RXo(kEdge), MY(527.0f), T_LABEL, true,
                  kWhiteR * ldim, kWhiteG * ldim, kWhiteB * ldim);
        float rlab_w = fmaxf(text_width("RC", T_LABEL), text_width("VIDEO", T_LABEL));
        float rx0    = RXo(271.0f);
        float rtw    = (RXo(kEdge) - rlab_w - kLabelGap * UX) - rx0;
        draw_seg_track(rx0, MY(525.0f), rtw, 9.0f * U, kSegsMain,
                       have_rc ? rc : 0.0f, false, kCyanR, kCyanG, kCyanB,
                       0.70f * ldim, 1.00f * ldim, f_block);
        // Amber, then red, with the screen-edge warning (see link_warn_hold):
        // the track that explains the glow wears its colour.
        float vr = kLimeR, vg = kLimeG, vb = kLimeB;
        if (in.link_warn >= 2)      { vr = 1.0f; vg = 0.30f; vb = 0.25f; }
        else if (in.link_warn == 1) { vr = 1.0f; vg = 0.72f; vb = 0.15f; }
        draw_text("VIDEO", RXo(kEdge), MY(537.0f), T_LABEL, true,
                  vr * ldim, vg * ldim, vb * ldim);
        draw_seg_track(rx0, MY(534.0f), rtw, 4.0f * U, kSegsThin,
                       in.mcs_norm, false, vr, vg, vb,
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
        if (in.link_freq_mhz > 0) snprintf(freq, sizeof(freq), "%dMHz", in.link_freq_mhz);
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
            draw_text(tp, x, by, T_ROW, true, vtxr, vtxg, vtxb);
            char with_sep[56];
            snprintf(with_sep, sizeof(with_sep), "%s \xE2\x80\xA2 ", lead);
            draw_text(with_sep, x - text_width(tp, T_ROW), by, T_ROW, true, ltr, ltg, ltb);
        } else if (lead[0]) {
            draw_text(lead, x, BL(base, T_ROW), T_ROW, true, ltr, ltg, ltb);
        } else if (tp[0]) {
            char solo[24];
            snprintf(solo, sizeof(solo), "VTX %s", tp);
            draw_text(solo, x, BL(base, T_ROW), T_ROW, true, vtxr, vtxg, vtxb);
        }
    }

    // Goggle row: what this box is actually receiving and decoding. Mode,
    // bitrate and latency sit together because each is read against the other
    // two - 18 Mbps is generous at 1080p60 and thin at 1080p120, and neither
    // figure says anything about how long the picture takes to arrive.
    {
        // The framerate is measured, so it lands on 59 as often as 60. Snap it
        // to the nearest rate a camera actually sends when it is within a frame
        // or two - "1080p59" reads as a fault where "1080p60" reads as a mode,
        // and the difference between them is measurement noise.
        int fps = in.fps;
        {
            static const int kRates[] = { 24, 25, 30, 48, 50, 60, 90, 100, 120 };
            for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++)
                if (abs(fps - kRates[i]) <= 2) { fps = kRates[i]; break; }
        }
        char mode[24] = "--", rate[24] = "--", lat[24] = "--";
        if (in.v_h > 0 && fps > 0)    snprintf(mode, sizeof(mode), "%dp%d", in.v_h, fps);
        else if (in.v_h > 0)          snprintf(mode, sizeof(mode), "%dp", in.v_h);
        if (in.mbps > 0.05f)          snprintf(rate, sizeof(rate), "%.1fMbps", in.mbps);
        if (in.have_lat)              snprintf(lat, sizeof(lat), "%.0fms", in.lat_med_ms);
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
        draw_text(buf, x, BL(base, T_ROW), T_ROW, true, ltr, ltg, ltb);
    }
}

namespace {
// The demo clock, shared. The top strip asks whether the simulated link is up
// long before demo_fill runs, and two copies of "c > 10" reading two different
// t0 values is exactly the kind of disagreement that shows up as a flickering
// state word.
uint64_t g_demo_t0 = 0;
// Nine phases, eight seconds apiece, not a simple modular pattern - the
// sequence visits standby three times and idle twice at different points in
// the loop, because what follows an idle depends on WHERE it falls: the one
// at the wrap comes from a fresh connect (still active, one beat before it
// settles to standby), the other two go straight back to standby, the same
// shortcut a real reconnect takes. Armed only ever coincides with active and
// disarmed only ever coincides with standby - the FC only tells the VTX to
// save power while it isn't flying (see ar8030-power-and-standby-verified.md)
// - so there is no "disarmed, still active" phase except that one beat right
// after a connect. IDLE_1 vs IDLE_2 needs no special-casing here: it falls
// out of canopy_link_up_ latching once in the real renderer, regardless of
// which array slot asks for link-down.
constexpr float kDemoPhaseS = 8.0f;
constexpr int   kDemoPhases = 9;
constexpr float kDemoCycle  = kDemoPhaseS * (float)kDemoPhases;
//                                     0      1     2     3     4     5      6      7     8
constexpr bool kPhaseLink[kDemoPhases]    = {false, true, true, true, true, true, false, true, false};
constexpr bool kPhaseArmed[kDemoPhases]   = {false, false,false, true,false, true, false,false,false};
constexpr bool kPhaseStandby[kDemoPhases] = {false, false, true,false, true,false, false, true,false};

float demo_phase(uint64_t now) {
    if (!g_demo_t0) g_demo_t0 = now;
    return fmodf((float)(now - g_demo_t0) / 1000000.0f, kDemoCycle);
}
}  // namespace

bool OSD::demo_link_up() {
    int phase = (int)(demo_phase(get_time_us()) / kDemoPhaseS);
    return kPhaseLink[phase];
}

bool OSD::demo_ever_connected() {
    uint64_t now = get_time_us();
    demo_phase(now);   // ensures g_demo_t0 is initialized before it's read
    return (float)(now - g_demo_t0) / 1000000.0f >= kDemoPhaseS;
}

void OSD::demo_fill(CanopyIn& in) {
    uint64_t now = get_time_us();
    const float t = (float)(now - (g_demo_t0 ? g_demo_t0 : now)) / 1000000.0f;

    float c = demo_phase(now);
    int   phase   = (int)(c / kDemoPhaseS);
    float phase_t = c - (float)phase * kDemoPhaseS;   // seconds into this phase

    const bool link    = kPhaseLink[phase];
    const bool armed   = kPhaseArmed[phase];
    const bool standby = kPhaseStandby[phase];
    float ft = armed ? phase_t : 0.0f;      // seconds airborne, this phase

    // A throttle trace: mostly cruising, punctuated. Everything that responds
    // to throttle is derived from this one number, so current, sag, speed and
    // altitude move together the way they do in the air.
    float thr = 0.42f + 0.34f * sinf(ft * 0.55f) + 0.14f * sinf(ft * 2.3f);
    thr = fmaxf(0.0f, fminf(1.0f, armed ? thr : 0.0f));

    // Pack: 4.15V sagging under load across the one armed phase.
    float used  = armed ? fminf(1.0f, ft / kDemoPhaseS) : 0.0f;
    float rest  = 4.15f - 0.50f * used;
    in.bf.have_cell_v = link;
    in.bf.cell_v      = rest - 0.42f * thr;
    in.bf.have_pack_v = link;
    in.bf.pack_v      = in.bf.cell_v * 6.0f;             // a 6S pack
    in.bf.have_amps   = link;
    in.bf.amps        = 1.2f + 38.0f * thr * thr;

    in.bf.have_alt   = armed;
    in.bf.alt_m      = armed ? 12.0f + 44.0f * (0.5f + 0.5f * sinf(ft * 0.31f)) : 0.0f;
    in.bf.have_speed = armed;
    in.bf.speed_kmh  = armed ? 8.0f + 92.0f * thr : 0.0f;
    in.bf.have_mode  = link;
    snprintf(in.bf.mode, sizeof(in.bf.mode), "%s", "ACRO");
    // -2 is "nothing said either way", which is what a goggle with no link
    // actually knows. It suppresses the ARMED/DISARMED label rather than
    // asserting a state we cannot see.
    in.bf.arm        = link ? (armed ? 1 : 0) : -2;
    // Nothing above this line exists without a link, so all of it is gated on
    // one: a goggle with no aircraft has no voltage, no mode and no arm state
    // to show. Stamping telemetry fresh through the idle window would also
    // make have_aircraft true there, putting the idle state - the dim and the
    // IDLE label - out of reach in demo mode entirely.
    in.bf.stamp_us   = link ? now : 0;

    // The flight timer is ours, driven off the same arm transition the real
    // one uses, so the label behaves exactly as it will in the air.
    in.bf.have_timer = armed;
    in.bf.timer_s    = (int)ft;

    // Control link: strong, with a dip as the aircraft gets out to distance.
    float far = armed ? fminf(1.0f, ft / kDemoPhaseS) : 0.0f;
    in.bf.have_lq = link;
    in.bf.lq_pct  = (int)(99.0f - 26.0f * far + 3.0f * sinf(ft * 1.7f));
    if (in.bf.lq_pct > 100) in.bf.lq_pct = 100;
    if (in.bf.lq_pct < 0)   in.bf.lq_pct = 0;

    // Video link. The MCS ladder steps rather than slides, which is the whole
    // reason the gauge is drawn in rungs.
    in.link_up  = link;
    in.mcs_norm = fmaxf(0.14f, 1.0f - 0.72f * far);
    in.mcs_norm = floorf(in.mcs_norm * 7.0f + 0.5f) / 7.0f;
    in.quality  = fmaxf(0.05f, 0.95f - 0.65f * far);
    // The same rung thresholds as the real link: the bottom two rungs red,
    // the next two amber - so the demo runs amber, then red, as it flies out.
    {
        int rung = (int)(in.mcs_norm * 7.0f + 0.5f) - 1;
        in.link_warn = !link ? 0 : (rung <= 1) ? 2 : (rung <= 3) ? 1 : 0;
    }
    in.dist_m   = link ? (int)(4.0f + 420.0f * far) : -1;

    in.video_active = in.link_up;
    in.v_w = 1920; in.v_h = 1080; in.fps = 60;
    in.mbps       = 19.5f - 6.0f * far + 0.8f * sinf(t * 0.9f);
    in.have_lat   = true;
    in.lat_med_ms = 28.0f + 16.0f * far + 3.0f * sinf(t * 1.3f);
    snprintf(in.vtx_pwr, sizeof(in.vtx_pwr), "%s", "500mW");
    in.link_freq_mhz  = 5740;
    in.vtx_temp_valid = true;
    in.vtx_temp_c     = 41.0f + 9.0f * used;
    in.vrx_temp_c     = 46.0f + 6.0f * (0.5f + 0.5f * sinf(t * 0.12f));
    // A 3S pack sagging slowly, so the row can be judged without hardware.
    in.vrx_volts      = 12.1f - 0.9f * (0.5f + 0.5f * sinf(t * 0.03f));

    // The one field real telemetry sets that demo mode used to leave alone -
    // demo_fill replaces the real sources rather than joining them, so
    // without this line whatever OSD::vtx_low_power last held (false, absent
    // a real air unit) would just survive untouched through every phase.
    in.vtx_low_power = standby;
}

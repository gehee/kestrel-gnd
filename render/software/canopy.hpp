#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// The canopy's design, in one place for both of its renderers: the VRX Pro's
// GLES OSD (hud/hud_canopy.cpp) and the native Ascent's software HUD both read
// these, so the two cannot drift apart. Coordinates are the original 1200x620
// canopy mockup.
namespace canopy {
// How far past the screen edge the frame is drawn. The dynamic HUD translates
// the panels with the aircraft's accelerometer - about 0.025 frustum units per
// G, with spring overshoot on top - so a rail that begins exactly at the edge
// swings its own end into view on a hard manoeuvre. Everything anchored to the
// edge starts out here instead, comfortably past anything the spring reaches.
constexpr float kBleed = 60.0f;
// In-plane slant. Everything in a panel - the wedge, the rules, the tracks and
// every line of type - shares this one angle, rising towards the centre of the
// screen. The mockup gave the frame and the content slightly different slopes
// (1.8 degrees on the wedge's top edge against 3.4 on the text), which reads
// as a mistake rather than as depth: a blade is one object, and the moment two
// of its edges disagree the eye stops seeing it as one.
constexpr float kSlantRad = 0.05934f;   // 3.4 degrees
// Mockup y (from the top of its 620px frame) -> frustum y.
// The panels sat 24 design px clear of the bottom edge. Half that reads
// better - they are anchored to the frame, and a wide margin under them
// makes them look like they are floating rather than sitting on it.
constexpr float kDrop = 12.0f;
// Text sizes, times the UI scale: the units the GLES OSD's canvas_->text() takes.
constexpr float kTextBig   = 0.185f;   // 46px headline
constexpr float kTextUnit  = 0.082f;   // 19-20px unit / secondary figure
constexpr float kTextSmall = 0.055f;   // 13px unit suffix
constexpr float kTextRow   = 0.058f;   // 13px detail rows
constexpr float kTextLabel = 0.040f;   // 9px track names
// The flight timer and the ARMED/DISARMED word share the top rail's gap.
// Sized between the headline voltage and the current beside it: the timer
// is one of the readings, not a caption on the frame, and at anything
// smaller it was not legible in flight.
constexpr float kTextState = 0.125f;   // between kTextBig and kTextUnit
constexpr int kFontPixels = 40, kTextCellHeight = 64, kTextBaseline = 48;
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
// Link acquired. Not ambient motion - it marks the moment the radio finds the
// aircraft, which is a moment you are already watching for. The phases run
// outboard to inboard, the direction the perspective recedes, so the panel
// reads as arriving from off-screen rather than fading up where it already is.
constexpr float kLinkRuleUs  = 120000.0f;   // rules run in from the edges
constexpr float kLinkShadeUs = 260000.0f;   // ...then the ground fades up
constexpr float kLinkAllUs   = 420000.0f;   // ...then the blocks sweep in
// The wedge under each panel: its outline's x stops (mockup px from the edge),
// top and bottom at each, and the ground's alpha there. The last row's
// baseline is 584, so a floor at 612 left 28px of empty ground under it
// against 4px above the headline. Brought up to match, and the tip's apex
// follows the new mid-line.
constexpr float kWedgeX[]   = { -kBleed, 156.0f, 252.0f, 300.0f };
constexpr float kWedgeTop[] = { 462.0f, 462.0f, 462.0f, 526.0f };
constexpr float kWedgeBot[] = { 590.0f, 590.0f, 590.0f, 526.0f };
constexpr float kWedgeA[]   = {  0.66f,  0.34f,  0.11f,   0.0f };

// Where a mockup pixel lands in a panel's own space, before the panel yaw and
// the slant. Worked out in one place because the labels over the blades while
// a side is being picked (draw_canopy_pick) are drawn in it too.
struct CanopyFrame {
    float sd = 1.0f;          // the UI scale
    float UX = 0, U = 0;      // one mockup column, one mockup row
    float LX = 0, RX = 0;     // the screen edges
    float BY = 0;             // the bottom
    float MY(float y) const  { return BY + (620.0f - kDrop - y) * U; }
    float LXo(float x) const { return LX + x * UX; }    // inward from the left
    float RXo(float x) const { return RX - x * UX; }    // inward from the right
};

inline CanopyFrame canopy_frame(float frustum_w, float frustum_h, float overscan, float s, float depth) {
    CanopyFrame F;
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
    const float ct = std::cos(kTilt), st = std::sin(kTilt);
    const float D  = depth;
    const float target_x = frustum_w - overscan;
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
    // more of a 16:9 frame than of the 1200x620 it was drawn on.
    //
    // UI Scaling scales the whole canopy, both axes alike: type, rows, and the
    // lengths of its tracks, graphs and grounds, anchored where it is (the
    // screen edges and the bottom). It used to take a third of the setting,
    // and only for rows and type, so the panels kept their size while the text
    // in them changed - at 0.5x the type sat small in full-length bars. Larger
    // settings widen the wings towards the centre in proportion (about 23% of
    // the width each at 1.0x, so 2.0x brings them close to meeting).
    F.sd = s;
    const float base = (frustum_h * 2.0f) / 620.0f;
    F.UX = ((frustum_w * 2.0f) / 1200.0f) * F.sd / mag;  // columns
    F.U  = (base * F.sd) / mag;                          // rows
    F.LX = -ex;                                   // left screen edge
    F.RX =  ex;                                   // right screen edge
    F.BY = (-frustum_h + overscan) / mag;    // bottom
    return F;
}

struct Point { float x, y; };

// Project the same slant/yaw as OSD::draw_canopy_hud on the CPU. Used for
// host asset baking and dynamic software gauge positions, not video rendering.
inline Point project_world(int width, int height, bool left, float x, float y) {
    constexpr float depth = 3.2f;
    const float fh = std::tan(0.785f / 2) * depth;
    const float fw = fh * width / height;
    const auto f = canopy_frame(fw, fh, 0.02f, 1, depth);
    const float px = left ? f.LXo(56) : f.RXo(56), py = f.MY(502);
    const float angle = left ? kSlantRad : -kSlantRad;
    const float dx = x - px, dy = y - py;
    const float sx = px + dx * std::cos(angle) - dy * std::sin(angle);
    const float sy = py + dx * std::sin(angle) + dy * std::cos(angle);
    const float yaw = left ? 0.26f : -0.26f;
    const float mag = depth / (depth + sx * std::sin(yaw));
    return {width * (0.5f + sx * std::cos(yaw) * mag / (2 * fw)),
            height * (0.5f - sy * mag / (2 * fh))};
}

inline CanopyFrame screen_frame(int width, int height) {
    const float fh = std::tan(0.785f / 2) * 3.2f;
    return canopy_frame(fh * width / height, fh, 0.02f, 1, 3.2f);
}
inline Point project(int width, int height, bool left, float x, float y) {
    const auto f = screen_frame(width, height);
    return project_world(width, height, left, left ? f.LXo(x) : f.RXo(x), f.MY(y));
}

// Source texture corners TL, TR, BR, BL. Model-space x always grows to the
// right, even on the right wing: mirroring panel coordinates must not mirror
// the glyphs. Matches OSD::draw_text/draw_icon, including its baseline padding.
inline std::array<Point, 4> text_quad(int width, int height, bool left, float x,
                                    float y, float scale, float bitmap_width,
                                    float advance, bool right_align) {
    const auto f = screen_frame(width, height);
    float wx = left ? f.LXo(x) : f.RXo(x);
    if (right_align) wx -= advance * scale / kTextCellHeight;
    const float wy = f.MY(y), w = bitmap_width * scale / kTextCellHeight;
    return {project_world(width, height, left, wx, wy + scale),
            project_world(width, height, left, wx + w, wy + scale),
            project_world(width, height, left, wx + w, wy),
            project_world(width, height, left, wx, wy)};
}

// The readings both HUDs colour or word the same way.
//
// Temperatures (the VRX's and the VTX's) go amber, then red, the same on every
// theme, or it stops being a warning: a safety signal, not a hue that changes
// with taste. 0 none, 1 amber, 2 red.
constexpr float kTempWarnC = 70.0f, kTempHotC = 85.0f;
inline int temperature_alert(float c) { return c >= kTempHotC ? 2 : c >= kTempWarnC ? 1 : 0; }
// The receiver's own supply. Per-cell so it reads the same on any pack: the
// ADC gives a raw pack voltage and the cell count is inferred from it (3S/4S/6S
// are what a barrel jack sees in practice). Thresholds are the usual lithium
// ones - 3.5V/cell is "land soon", 3.3V is "now".
constexpr float kCellWarnV = 3.50f, kCellLowV = 3.30f;
inline int supply_cells(float v) {
    if (v <= 0.0f)  return 0;
    if (v < 9.0f)   return 2;
    if (v < 13.0f)  return 3;
    if (v < 17.5f)  return 4;
    if (v < 21.5f)  return 5;
    return 6;
}
// 0 none, 1 amber, 2 red, by the volts per cell.
inline int cell_alert(float per_cell) { return per_cell <= kCellLowV ? 2 : per_cell <= kCellWarnV ? 1 : 0; }
// The framerate is measured, so it lands on 59 as often as 60. Snapped to the
// nearest rate a camera actually sends when it is within a frame or two -
// "1080p59" reads as a fault where "1080p60" reads as a mode, and the
// difference between them is measurement noise.
inline int snapped_fps(int fps) {
    static constexpr int kRates[] = { 24, 25, 30, 48, 50, 60, 90, 100, 120 };
    int best = fps, delta = 3;
    for (int r : kRates) if (std::abs(fps - r) < delta) { delta = std::abs(fps - r); best = r; }
    return best;
}
// The recording's running time where the panel shows it.
inline int recording_label(char* out, size_t n, unsigned seconds) {
    return std::snprintf(out, n, "REC %u:%02u", seconds / 60, seconds % 60);
}
// The video link warning's screen-edge glow: it fades in fast and out slowly
// (a, 0..1, towards target), and red breathes so it cannot be mistaken for a
// tint in the footage. dt is the time since the last frame, seconds. At rest
// (fully off or fully on) a HUD draws seldom, so that time is no measure of
// the fade: it starts with one animation frame, not a 100 ms jump.
constexpr float kEdgeInS = 0.15f, kEdgeOutS = 0.60f;
inline float edge_fade(float a, float target, float dt) {
    if (dt > 0.1f) dt = 0.1f;
    if ((a <= 0.001f || a >= 0.999f) && dt > 1.0f / 60.0f) dt = 1.0f / 60.0f;
    const float step = (target > a ? 1.0f / kEdgeInS : 1.0f / kEdgeOutS) * dt;
    return a < target ? std::min(target, a + step) : std::max(target, a - step);
}
// Red's breathing, as a factor on its alpha: t is any microsecond clock.
inline float edge_pulse(uint64_t t_us) {
    return 0.65f + 0.35f * (0.5f + 0.5f * std::sin((float)(t_us % 10000000ULL) / 1e6f * 7.54f));
}

struct LinkVisual {
    bool visible = false, animating = false;
    float idle_dim = 1, rail_dim = 1, mark = 0;
    float rule = 1, shade = 1, block = 1;
};

// The link-acquired sweep and the link-loss dim/pulse/IDLE, as both HUDs
// draw them. Both entries into CONNECTED replay the same rebuild sweep: one
// gesture for "the link is back" is worth more than two that have to be told
// apart mid-flight. The state (previous, up, down) is the caller's: the GLES
// OSD keeps it in its members.
//   idle_dim   content and ground brightness, 1 -> kIdleFloor
//   rail_dim   the same, but with the pulse riding over it (one pulse,
//              returning to the floor it started from)
//   mark       IDLE fading in and the rail gap opening under it
//   rule/shade/block  the rebuild: rails run in from the edge, the ground
//              fades up behind them, then the blocks arrive
// animating: a phase is still running, and the caller must draw again.
inline LinkVisual link_visual(bool connected, uint64_t now, bool& previous,
                              uint64_t& up, uint64_t& down) {
    if (connected != previous) (connected ? up : down) = now;
    previous = connected;
    LinkVisual v;
    if (!up) return v;
    v.visible = true;
    if (!connected) {
        v.idle_dim = v.rail_dim = kIdleFloor;
        v.mark = 1;
        const float e = down && now >= down ? float(now - down) : kIdleAllUs;
        if (e < kIdleAllUs) {
            v.animating = true;
            v.idle_dim = 1 - (1 - kIdleFloor) * std::min(1.0f, e / kIdleBriUs);
            v.rail_dim = v.idle_dim;
            const float pt = (e - kIdleBriUs) / kIdlePulseUs;
            if (pt > 0 && pt < 1) v.rail_dim = kIdleFloor + (1 - kIdleFloor) * std::sin(pt * 3.14159265f);
            v.mark = std::clamp((e - kIdleMarkAt) / kIdleMarkUs, 0.0f, 1.0f);
        }
    } else down = 0;
    const float e = now >= up ? float(now - up) : 0;
    if (e < kLinkAllUs) {
        v.animating = true;
        v.rule = std::min(1.0f, e / kLinkRuleUs);
        v.shade = std::clamp((e - kLinkRuleUs) / (kLinkShadeUs - kLinkRuleUs), 0.0f, 1.0f);
        v.block = std::clamp((e - kLinkShadeUs) / (kLinkAllUs - kLinkShadeUs), 0.0f, 1.0f);
    }
    return v;
}
} // namespace canopy

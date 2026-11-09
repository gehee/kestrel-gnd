#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Shared with the GLES OSD. Coordinates are the original 1200x620 canopy
// mockup, not a second design maintained by the software frontend.
namespace canopy {
constexpr float kBleed = 60.0f;
constexpr float kSlantRad = 0.05934f;
constexpr float kDrop = 12.0f;
constexpr float kTextBig = 0.185f, kTextUnit = 0.082f, kTextSmall = 0.055f;
constexpr float kTextRow = 0.058f, kTextLabel = 0.040f, kTextState = 0.125f;
constexpr int kFontPixels = 40, kTextCellHeight = 64, kTextBaseline = 48;
constexpr float kIdleBriUs = 240000.0f;
constexpr float kIdlePulseUs = 360000.0f;
constexpr float kIdleMarkAt = 440000.0f;
constexpr float kIdleMarkUs = 240000.0f;
constexpr float kIdleAllUs = 800000.0f;
constexpr float kIdleFloor = 0.5f;
constexpr float kLinkRuleUs = 120000.0f;
constexpr float kLinkShadeUs = 260000.0f;
constexpr float kLinkAllUs = 420000.0f;
constexpr float kWedgeX[] = {-kBleed, 156.0f, 252.0f, 300.0f};
constexpr float kWedgeTop[] = {462.0f, 462.0f, 462.0f, 526.0f};
constexpr float kWedgeBot[] = {590.0f, 590.0f, 590.0f, 526.0f};
constexpr float kWedgeA[] = {0.66f, 0.34f, 0.11f, 0.0f};

struct CanopyFrame {
    float sd = 1, UX = 0, U = 0, LX = 0, RX = 0, BY = 0;
    float MY(float y) const { return BY + (620.0f - kDrop - y) * U; }
    float LXo(float x) const { return LX + x * UX; }
    float RXo(float x) const { return RX - x * UX; }
};

inline CanopyFrame canopy_frame(float fw, float fh, float overscan, float scale, float depth) {
    const float ct = std::cos(0.26f), st = std::sin(0.26f);
    const float target = fw - overscan;
    const float ex = target * depth / (ct * depth + target * st);
    const float mag = depth / (depth - st * ex);
    return {scale, fw * 2 / 1200 * scale / mag, fh * 2 / 620 * scale / mag,
            -ex, ex, (-fh + overscan) / mag};
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

struct LinkVisual {
    bool visible = false, animating = false;
    float idle_dim = 1, rail_dim = 1, mark = 0;
    float rule = 1, shade = 1, block = 1;
};

// Original OSD link-acquired sweep and link-loss dim/pulse/IDLE timing.
// References let the legacy OSD retain its existing state and call sites.
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

#pragma once
// The HUD's settings (see Setting, base/settings.hpp).

#include "base/settings.hpp"

namespace hud::setting {

// The HUD: OFF, CANOPY, ARENA, ARENA FULL (hud_overlay; -1: not chosen yet,
// then the older hud_detail and hud_style decide it, once).
inline constexpr Setting<int> hud_overlay{"hud_overlay", -1};
inline constexpr Setting<int> hud_detail{"hud_detail", 2};   // older: 0 MINIMAL 1 MEDIUM 2 FULL
inline constexpr Setting<int> hud_style{"hud_style", 1};     // older: 0 LEGACY 1 CANOPY
inline constexpr Setting<int> hud_theme{"hud_theme", 0};
// The Betaflight OSD on or off (-1: not chosen yet, then the older msp_mode).
inline constexpr Setting<int> bf_osd{"bf_osd", -1};
inline constexpr Setting<int> msp_mode{"msp_mode", 1};
inline constexpr Setting<int> volt_mode{"volt_mode", 0};     // 0 the pack, 1 per cell
// HUD > Reactivity, 0 OFF .. 3 EXTREME (it was a bool once: "true"/"false"
// are read as 2 and 0).
inline constexpr Setting<int> hud_reactivity{"hud_reactivity", 2};
inline constexpr Setting<bool> hud_debug{"hud_debug", false};
inline constexpr Setting<bool> link_edge_warning{"link_edge_warning", true};
// How far up the air unit's camera is tilted on the airframe, degrees: found
// at each arming, saved for the next flight.
inline constexpr Setting<int> imu_cam_tilt_deg{"imu_cam_tilt_deg", 0};
// The clock: when it shows (clock_show, -1: not chosen yet) and in which
// format (clock_mode); older still, show_clock.
inline constexpr Setting<int> clock_show{"clock_show", -1};
inline constexpr Setting<int> clock_mode{"clock_mode", -1};
inline constexpr Setting<bool> show_clock{"show_clock", true};
// DISPLAY > Brightness, Picture Scale (percent), UI Scale.
inline constexpr Setting<int> brightness{"brightness", 50};
inline constexpr Setting<int> picture_scale{"picture_scale", 100};
inline constexpr Setting<float> ui_scale{"ui_scale", 1.0f};
// The menu's background clip, the canopy's antialiased edges, 4x MSAA.
inline constexpr Setting<bool> bg_video_enabled{"bg_video_enabled", true};
inline constexpr Setting<bool> hud_feather{"hud_feather", true};
inline constexpr Setting<bool> hud_msaa{"hud_msaa", false};
// The OSD at all, its plane's z-position, its stats period (ms), and the
// stats on the console.
inline constexpr Setting<int> enable_osd{"enable_osd", 1};
inline constexpr Setting<int> osd_zpos{"osd_zpos", 2};
inline constexpr Setting<int> osd_refresh{"osd_refresh", 1000};
inline constexpr Setting<bool> stats{"stats", false};

}  // namespace hud::setting

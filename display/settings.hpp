#pragma once
// The display's settings (see Setting, base/settings.hpp).

#include <string>

#include "base/settings.hpp"

namespace display::setting {

// DISPLAY > Screen Mode, the screen's own (by the ID in its EDID; "" for a
// screen with none): "WxH@Hz", unset for Auto. A mode picked in the menu is
// on trial the first time: screen_mode_try names the screen and the mode,
// and is taken back at the next start (app/main.cpp).
inline MadeSetting<std::string> screen_mode(const std::string& screen_id) {
    return {screen_id.empty() ? std::string("screen_mode") : "screen_mode_" + screen_id, ""};
}
inline constexpr Setting<std::string> screen_mode_try{"screen_mode_try", ""};
// DISPLAY > Refresh Sync, the screen's own too: 0 OFF, 1 ON, 2 FREE.
inline MadeSetting<int> display_lock(const std::string& screen_id) {
    return {screen_id.empty() ? std::string("display_lock") : "display_lock_" + screen_id, 0};
}
// DISPLAY > Sync To: 1 the first slice, 2 the last. Sync Margin, ms.
inline constexpr Setting<int> display_lock_slice{"display_lock_slice", 1};
inline constexpr Setting<int> display_lock_margin_ms{"display_lock_margin_ms", 0};
// The camera's own frame period as measured, one per video mode
// ("camera_period_us_1080p100"): Screen Mode Auto trims the screen's clock
// to it (DrmDevice), straight away for a mode seen before.
inline MadeSetting<float> camera_period(int lines, int fps) {
    return {"camera_period_us_" + std::to_string(lines) + "p" + std::to_string(fps), 0.0f};
}

// The renderer: each commit this long before the vblank it is for, and a
// picture less than the slack from one waits for the next; off, a commit
// at once (the deadline commit is what Refresh Sync times).
inline constexpr Setting<int> deadline_commit{"deadline_commit", 1};
inline constexpr Setting<float> commit_margin_ms{"commit_margin_ms", 1.2f};
inline constexpr Setting<float> commit_min_slack_ms{"commit_min_slack_ms", 0.6f};
// Under VRR, pictures in halves (two commits each).
inline constexpr Setting<int> vrr_halves{"vrr_halves", 1};
// The low-latency switches (app/main.cpp): a picture up before it is all
// decoded, and pictures in halves on two planes.
inline constexpr Setting<int> early_present{"early_present", 1};
inline constexpr Setting<int> video_halves{"video_halves", 1};
// VSync (atomic commits) or not; no display at all (benchmarks).
inline constexpr Setting<bool> vsync{"vsync", false};
inline constexpr Setting<bool> no_display{"no_display", false};
inline constexpr Setting<int> video_zpos{"video_zpos", 1};
// VRR (FreeSync over HDMI): opt-in on the goggle, where it needs fpvOS's
// kernel and a sink that declares it.
#if defined(__aarch64__) || defined(__arm__)
constexpr bool kVrrDefault = false;
#else
constexpr bool kVrrDefault = true;
#endif
inline constexpr Setting<bool> enable_vrr{"enable_vrr", kVrrDefault};

}  // namespace display::setting

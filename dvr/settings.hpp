#pragma once
// The DVR's settings (see Setting, base/settings.hpp).

#include <string>

#include "base/settings.hpp"

// What REC records when the settings do not say (dvr_screen): the screen -
// video and HUD as the pilot sees them - 60 times a second.
constexpr bool kDvrScreenDefault    = true;

namespace dvr::setting {

// Where the recordings go (the recordings partition, made on first boot).
inline constexpr Setting<std::string> dvr_dir{"dvr_dir", "/media/dvr"};
// DVR > Source: the screen (true) or the FPV stream.
inline constexpr Setting<bool> dvr_screen{"dvr_screen", kDvrScreenDefault};
// A screen recording's codec ("h265" or "h264"), its ceiling in frames a
// second, and its bitrate, kbps.
inline constexpr Setting<std::string> dvr_screen_codec{"dvr_screen_codec", "h265"};
inline constexpr Setting<int> dvr_screen_fps{"dvr_screen_fps", 100};
inline constexpr Setting<int> dvr_screen_kbps{"dvr_screen_kbps", 30000};
// The --dvr recording (a file from the start, not the REC button's): its
// file, format ("mp4", "fmp4", "raw") and frame rate (0: the stream's).
inline constexpr Setting<std::string> dvr{"dvr", ""};
inline constexpr Setting<std::string> dvr_format{"dvr_format", "fmp4"};
inline constexpr Setting<int> dvr_framerate{"dvr_framerate", 0};
// The web view's screen encoder: frames a second, and its ceiling, kbps.
inline constexpr Setting<int> web_fps{"web_fps", 30};
inline constexpr Setting<int> web_kbps{"web_kbps", 5000};

}  // namespace dvr::setting

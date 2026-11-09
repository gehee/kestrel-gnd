#pragma once
// What the files of the OSD (hud/osd.hpp) share and nothing else includes:
// their includes, and the tables the menu's rows, the keys and the radio's
// reports read their values from.
//
// The OSD is one class, its methods in a file per concern:
//   osd.cpp             construction, the HUD's thread (run) and its pacing
//   osd_images.cpp          its GL: buffers, textures, shaders, the background,
//                       handing each frame to the display
//   osd_frame.cpp       a frame: render_frame(), and what it takes and works out
//                       once for the screens (the Frame)
//   osd_video.cpp       what the renderer reports (VideoListener) and the
//                       link figures it keeps
//   osd_radio.cpp       the radio and the air unit, as the HUD takes them
//   osd_menu_rows.cpp   the menu's rows: what each lists, shows and does
//   osd_keys.cpp        the keys
// and the screens, each drawing from the Frame: hud_idle.cpp (the idle
// screen, the warp, acquiring), hud_strip.cpp (the shade, the top strip, the
// edge glow), hud_lock.cpp (the lock, the bind), hud_arena.cpp, hud_canopy.cpp,
// hud_betaflight.cpp, hud_menu.cpp (the canopy awake: the menu); and the
// channel page (osd_channel.cpp), the gallery (osd_gallery.cpp), the
// pixel-space pages (osd_screen.cpp).

#include <poll.h>
#include <sys/statvfs.h>
#include "base/sd_log.hpp"
#include "display/settings.hpp"
#include "dvr/settings.hpp"
#include "hud/settings.hpp"
#include "base/frame_mode.hpp"
#include "hud/osd.hpp"

// An idle-screen asset's path (osd_set_assets_dir).
std::string osd_asset(const char* name);
#include "base/restart.hpp"
#include "hud/hud_theme.hpp"
#include "dvr/dvr.hpp"
#include "hal/wifi_ap.hpp"
#include "hal/picture.hpp"
#include "base/settings.hpp"
#include "region/zones.hpp"
#include "system/air/camera.hpp"
#include "system/air/encoder.hpp"
#include <iostream>
#include <unistd.h>
#include <algorithm>
#include <set>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <cairo.h>

#include "base/time_util.h"
#include <ctime>   // the top-centre clock: time/localtime_r/strftime
#include "utils/math_utils.hpp"
#include "msp/betaflight_glyphs.hpp"
#include "kestrel_gnd_config.h"
#include "bsp/bsp.hpp"
#include "platform/platform.hpp"

// The menu's sections, by menu_tab: drawn on the tab strip, named in the log.
inline constexpr const char* kTabNames[8] = { "VIDEO", "RADIO", "HUD", "DISPLAY", "DVR", "SYSTEM", "INFO", "SERIAL" };

// AR8030 RF menu. The rows are Channel (the channel page, where AUTO is the
// old Channel Hop), TX Power and Standby Mode; MCS, Bandwidth and LNA Mode were
// removed - see ar_rf_items().
//
// Power levels live in system/air/power.hpp (kArPwrLevels): mW, with the dBm the radio
// wants and stock's "N+1 means auto capped at N" encoding, taken from
// ar_ldy_gnd's own table. Capped at 31 dBm - stock's table goes to 37 but
// bb_set_pwr_in_t documents [0-31], and the board browns out under load
// (section 11).
// Stock's work-channel table (kHz), plus AUTO scan/hop at index 0.
static const int kArChanVals[]   = { -1, 5740000, 5770000, 5805000, 5839000 };
static const char* kArChanLabels[] = { "AUTO", "5740", "5770", "5805", "5839" };
static const int kArChanCount = 5;
// Max Bitrate's choices: system/air/encoder.hpp (the Ascent's menu offers them too).
using air::kArMaxBrVals; using air::kArMaxBrLabels; using air::kArMaxBrCount; using air::ar_maxbr_index;
// Max Bandwidth: how wide the video link may go. The goggle's radio widens it
// by itself on a good link (bw_auto), to 40 MHz with fpvOS's radio config -
// more throughput and a shorter frame, less range - or 20 with its 20 MHz one
// (bb_only.sh picks it from ar8030_max_bw_mhz). Any air unit follows. With
// Open8030 on the goggle, AUTO (the radio's own config, nothing sent), 5 and
// 10 too, taken live (SET 22).
static const int kArMaxBwVals[]     = { 0, 5, 10, 20, 40 };
static const char* kArMaxBwLabels[] = { "AUTO", "5 MHz", "10 MHz", "20 MHz", "40 MHz" };
static const int kArMaxBwCount = 5;
static inline int ar_maxbw_index(int mhz) { return mhz == 0 ? 0 : mhz == 5 ? 1 : mhz == 10 ? 2 : mhz == 20 ? 3 : 4; }
// Max MCS (Open8030 on the goggle): the highest MCS its receive policy may
// use - AUTO (none), or MCS 0..11. Lower: fewer blocks sent again (the radio
// part's latency tail), less throughput.
static const int kArMaxMcsCount = 13;   // AUTO, MCS 0..11
static inline const char *ar_maxmcs_label(int i) {
    static char b[8];
    if (i <= 0) return "AUTO";
    snprintf(b, sizeof(b), "MCS %d", i - 1);
    return b;
}
// Radio Frame: the TDD frame mode asked of the radio (Ar8030Link::frame_mode).
// SHORT, mode 2, is 3.25 ms at 40 MHz and 4.65 at 20; STANDARD, stock's mode
// 0, is 4.23 and 5.63, with an uplink twice as long. For field tests.
static const int kArFrameVals[]     = { 2, 0 };
static const char* kArFrameLabels[] = { "SHORT", "STANDARD" };
static const int kArFrameCount = 2;
static inline int ar_frame_index(int mode) { return mode == 0 ? 1 : 0; }
// VRX Radio Firmware: the baseband firmware the goggle's radio runs (bb_only.sh
// uploads it from ar8030_firmware): Open8030 when it is installed (the
// default), or stock's.
static const char* kArFwLabels[] = { "STOCK", "OPEN8030" };
static const int kArFwCount = 2;
// DISPLAY > Refresh Sync's values (display::RefreshSync::mode).
static inline const char *refresh_sync_label(int v) {
    return v == 2 ? "FOLLOW" : v == 1 ? "NUDGE" : "OFF";
}

// The camera's menu choices (EV, scene, 3D DNR, white balance, rotation,
// saturation/sharpness/contrast): system/air/camera.hpp, which the native
// Ascent's menu offers too.
using air::kEvSteps; using air::kEvLabels; using air::kEvCount;
using air::kSceneLabels; using air::kSceneVals; using air::kSceneCount;
using air::kDnrLabels; using air::kDnrVals; using air::kDnrCount;
using air::kWbCount; using air::wb_kelvin; using air::wb_label;
using air::kRotateLabels;
using air::kCamSatMax; using air::kCamSharpMax; using air::kCamContrastMax; using air::cam_auto_label;


static inline int freq_to_channel(int freq) {
    if (freq == 2484) return 14;
    if (freq < 2484) return (freq - 2407) / 5;
    if (freq >= 4910 && freq <= 4980) return (freq - 4000) / 5;
    if (freq < 5935) return (freq - 5000) / 5;
    if (freq >= 5955 && freq <= 7115) return (freq - 5950) / 5; // 6GHz
    return 0; 
}

// The Video tab's rows as the handlers below number them, from where they sit
// on screen: Max Bitrate (10) is listed second, right after Video Mode.
static inline int video_row(int shown) {
    if (shown == 1) return 10;
    return shown >= 2 && shown <= 10 ? shown - 1 : shown;
}

// The radio's MCS rungs, and the SNR margin each needs before it steps down
// (dB; measure_link and the arena's LINK row).
static const int   kMcsLadder[] = { 1, 2, 5, 7, 8, 10, 12 };
static const float kMcsDownDb[] =
    { 0.29f, 0.65f, 1.19f, 2.41f, 4.75f, 9.47f, 14.66f };  // per rung

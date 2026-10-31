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
// Max Bitrate: kestrel-air's video bitrate cap, kbps (0 = AUTO: it follows the
// link, up to 20 Mbps; a cap above that lets kestrel-air go to it, 40 at most,
// on a link that carries it - 40 MHz). Below the link's own rate it buys a
// steadier picture and shorter slices on the air; stock air units ignore it.
static const int kArMaxBrVals[]     = { 0, 6000, 8000, 10000, 12000, 15000, 18000, 20000, 22000, 25000,
                                        30000, 35000, 40000 };
static const char* kArMaxBrLabels[] = { "AUTO", "6 Mbps", "8 Mbps", "10 Mbps", "12 Mbps",
                                        "15 Mbps", "18 Mbps", "20 Mbps", "22 Mbps", "25 Mbps",
                                        "30 Mbps", "35 Mbps", "40 Mbps" };
static const int kArMaxBrCount = 13;
static inline int ar_maxbr_index(int kbps) {
    for (int i = 0; i < kArMaxBrCount; i++) if (kArMaxBrVals[i] == kbps) return i;
    return 0;
}
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
    return v == 2 ? "FREE" : v == 1 ? "ON" : "OFF";
}

// AR8030 camera menu option tables. EV, the mode list and the anti-flicker
// choices come from the stock GlassesUI; contrast is 0-15 because the config
// frame packs it into a nibble. Saturation/sharpness ranges are NOT confirmed -
// the camera acks every setting, so watch the log for a non-zero status.
static const int   kEvSteps[]      = { -10, -7, -3, 0, 3, 7, 10 };
static const char* kEvLabels[]     = { "-1.0", "-0.7", "-0.3", "0", "+0.3", "+0.7", "+1.0" };
static const int   kEvCount        = 7;
// Scene. Captured off the wire from stock (cmd 0x01): Race = 0, Standard = 2.
// We used to send the menu index (0 = Standard, 1 = Race), i.e. both values
// wrong. The gap at 1 is presumably a scene this unit does not offer.
static const char* kSceneLabels[]  = { "Race", "Standard" };
static const int   kSceneVals[]    = { 0, 2 };
static const int   kSceneCount     = 2;
// 3D DNR, sky cmd 0x1B. The stock goggle sends Off/Low/Mid/High/Auto as
// 1/2/3/4/5, but the air unit - the stock app and kestrel-air alike - takes
// 0 off, 1 its tuning bin's own noise reduction, 2/3/4 low/mid/high, and
// anything else as 1: stock's Off (1) left it on. Off is 0 here, which both
// turn off; Auto stays 5, the bin's.
static const char* kDnrLabels[]    = { "Off", "Low", "Mid", "High", "Auto" };
static const int   kDnrVals[]      = { 0, 2, 3, 4, 5 };
static const int   kDnrCount       = 5;
// Anti-Flicker (sky cmd 0x1F) was here but is gone: stock's own Camera menu
// has no such row at all - confirmed against a photo of the stock UI - so it
// was never a real setting to expose, whatever encoding the command itself
// takes on the wire. See CMD_SET_ANTI_FLICKER in ar8030_sky.hpp.
// White balance. The wire carries the colour temperature in Kelvin (cmd 0x05),
// and stock offers Auto plus 4000K..7000K in 100K steps - 32 entries. The old
// five-value table was a guess and did not match any of stock's steps except
// 4000K. fact_env.json stores the *index*, which is why 7000K reads as 31.
static const int   kWbCount        = 32;
static inline int  wb_kelvin(int i) { return i <= 0 ? 0 : 3900 + i * 100; }
static inline const char* wb_label(int i) {
    static char buf[8];
    if (i <= 0) return "Auto";
    snprintf(buf, sizeof(buf), "%dK", wb_kelvin(i));
    return buf;
}
// Camera rotation, stock's Camera > Rotate row (sky cmd 0x06).
static const char* kRotateLabels[] = { "0", "180" };
// Saturation, sharpness and contrast are Auto plus 1..10 on stock, and 0 is
// what Auto puts on the wire - the same scheme white balance uses (0 = Auto,
// otherwise a real value). Rendering 0 as "0" made Auto look like "minimum".
// Contrast used to allow 15: that is the width of the nibble the config body
// packs it into, not stock's range.
static const int   kCamSatMax      = 10;
static const int   kCamSharpMax    = 10;
static const int   kCamContrastMax = 10;
// 0 -> "Auto", anything else the number itself.
static inline const char* cam_auto_label(int v) {
    static char buf[8];
    if (v <= 0) return "Auto";
    snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}


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

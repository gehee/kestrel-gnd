#ifndef OSD_H
#define OSD_H

#include <atomic>
#include <deque>
#include <vector>
#include <mutex>
#include <memory>
#include <map>

#include "base/settings.hpp"
#include "display/display.hpp"
#include "display/hud_layer.hpp"
#include "display/video_listener.hpp"
#include "icons/icons.h"
#include "base/scheduling_helper.hpp"
#include "base/prof.hpp"
#include <functional>
#include "system/air/power.hpp"
#include "utils/math_utils.hpp"
#include "msp/msp_osd.hpp"
#include "hud/bg_video.hpp"
#include "hud/gallery.hpp"
#include "hud/gallery_stats.hpp"
#include "hud/link_view.hpp"
#include "hud/parts.hpp"
#include "render/canvas.hpp"

namespace render { class CanvasGles; }

namespace radio { struct State; }

struct DecodedUnit;
class DVR;

// Kick off background-asset decoding (background.png + idle "KESTREL" title) on
// a worker thread so the heavy Cairo/PNG work overlaps DRM/EGL driver bring-up.
// Call once, early in main(), before the display is made. init_images()
// consumes the results (and falls back to a synchronous load if not ready).
void osd_prefetch_assets_async();
// Where the idle screen's assets are (background.png, background.mp4):
// /etc/kestrel unless set, before the prefetch and the OSD.
void osd_set_assets_dir(const std::string& dir);

// The link figures the HUD keeps (osd_vars) - wfb's, and the air unit's
// adaptation as its status channel reports it.
typedef struct  {
    uint16_t freq;
    uint8_t bandwidth;
    uint8_t mcs_index;
    int8_t rssi_min;
    int8_t rssi_max;
    int8_t rssi_avg;
    int8_t snr_min;
    int8_t snr_max;
    int8_t snr_avg;
} antenna_stats;

typedef struct  {
    uint32_t count_p_all;
    uint32_t count_p_fec_recovered;
    uint32_t count_p_lost;
    uint32_t count_p_duped;
    uint32_t p_all_rate;
    uint32_t p_fec_recovered_rate;
    uint32_t p_lost_rate;
    uint32_t p_duped_rate;
    uint32_t bandwidth_data_bps;
    uint32_t bandwidth_parity_bps;
    uint8_t fec_k;
    uint8_t fec_n;
    double wfb_fec_rate;
    std::vector<antenna_stats> antennas;
} packets_stats;

typedef struct png_closure
{
	unsigned char * iter;
	unsigned int bytes_left;
} png_closure_t;

#include "hud/latency_ring.hpp"
#include "base/bus.hpp"
#include "dvr/format_card.hpp"

// OSD Vars
struct osd_vars {

	// Video Decoder
	int current_framerate;
    bool enable_latency;
	float proc_latency_avg, proc_latency_min, proc_latency_max;
    std::vector<float> proc_latency_values;
	float decoding_latency_avg, decoding_latency_min, decoding_latency_max;
    std::vector<float> decoding_latency_values;
	float display_latency_avg, display_latency_min, display_latency_max;
    std::vector<float> display_latency_values;
    float tx_latency_avg, tx_latency_min, tx_latency_max;
    std::vector<float> tx_latency_values;
	float total_latency_avg, total_latency_min, total_latency_max;
    std::vector<float> total_latency_values;
	float frame_pace_avg, frame_pace_min, frame_pace_max;
    std::vector<float> frame_pace_values;
    float proc_latency_worst, decoding_latency_worst, display_latency_worst, tx_latency_worst;
	float video_bandwidth;
	uint32_t video_width;
	uint32_t video_height;

	// WFB-ng
    packets_stats link_stats;
    bool slices_received;

    // Settings
    uint8_t settings_mcs_index;
    uint8_t settings_power_lvl;
    unsigned int settings_bandwidth;
    char settings_mode;
    float ui_scale;

    // History for graphs
    std::vector<float> latency_history;
    std::vector<float> bitrate_history;
    LatencyRing latency_ring;               // see hud/latency_ring.hpp
    uint64_t latency_history_last_us = 0;   // last real/gap append (graph gap-filler)
    gnd::LinkStats radio_link;
    gnd::ScanSample chan_scan{}; // per-channel spectrum scan (channel-scan screen)
};




// Open the settings menu at startup (--menu). See the member for why.
void osd_set_menu_at_start(bool on);
void osd_set_menu_start_tab(int t);

class OSD : public display::VideoListener {
    private:
        Settings& settings_;   // the HUD's and the menu's settings, and those its rows change
        std::string dvr_dir_;  // where the recordings are (DVR > Folder)
        std::shared_ptr<display::Display> dev;
        // Refresh Sync's state: the defaults without a display.
        display::RefreshSync refresh_sync() const { return dev ? dev->refresh_sync() : display::RefreshSync{}; }

        // State
        struct osd_vars osd_vars = {0};
        pthread_mutex_t osd_mutex;
        bool console_stats = false;
        std::atomic<bool>* signal_stop;
        // The HUD thread sleeps on this (OSD::run): a poke needs no lock and is
        // never lost, so the renderer and the radio wake it without osd_mutex.
        bus::Waker hud_wake_;
        std::atomic<bool> render_requested{false};
        // Where the frames go (display/hud_layer.hpp), made by the platform;
        // ready once init_images() has made its buffers - nothing is drawn
        // before, or if it could not.
        std::unique_ptr<display::HudLayer> layer_;
        bool layer_ready_ = false;

        
        // Textures
        render::ImageId fps_tex, lat_tex, net_tex, logo_tex, bg_tex;
        std::map<int, render::ImageId> rssi_texs;
        std::map<int, render::ImageId> link_texs;
        
        MspOsd msp_osd;
        render::ImageId char_tex_cache[1024] = {0};
        int char_tex_w_ = 0, char_tex_h_ = 0;   // the pixel size char_tex_cache was rendered at
        int   char_w_px_[1024] = {0};           // each cached texture's width, pixels
        float char_adv_px_[1024] = {0};         // how far the pen moves after it: a letter's own advance, an icon's cell

        float rotation_y = 0.0f;
        float wrap_depth = 3.2f;

        // --- Background video + warp-zoom transition ---
        enum class VideoState { BACKGROUND, TRANSITION, LIVE };
        // Written by the renderer (BACKGROUND -> TRANSITION, under osd_mutex)
        // and the HUD; the renderer reads it without the lock.
        std::atomic<VideoState> video_state_{VideoState::BACKGROUND};
        std::atomic<uint64_t> last_fpv_frame_us_{0};   // last frame notify timestamp (the renderer)
        uint64_t   transition_start_us_  = 0;   // when warp zoom started
        uint64_t   last_bg_signal_us_    = 0;   // throttles idle bg-video redraws
        // The fpvOS warp-zoom into live video. Was disabled at 0 (974c86e):
        // it was written for a handoff out of the idle title, and the
        // acquiring screen claimed the same 450ms window, so the two fought
        // over it. Fixed properly now - acquiring and holding both exclude
        // VideoState::TRANSITION (render_frame) instead of the warp being turned
        // off - so this can carry the real duration again.
        static constexpr float TRANSITION_DURATION_US = 450000.0f;
        // The first connection's warp waits for the video to pass this rate
        // (or 2 below the Max Bitrate cap, if lower), or for the timeout after
        // its first clean picture (notify_video_frame).
        static constexpr float    kRevealMbps      = 5.0f;
        static constexpr uint64_t kRevealTimeoutUs = 5000000;
        uint64_t reveal_wait_us_ = 0;   // the first clean picture of the wait, 0 = none yet

        BgVideoPlayer* bg_player_ = nullptr;

        // Software decode: an image the frames are copied into (BGRA)
        render::ImageId         bg_video_tex_      = 0;

        // Hardware decode: a video image, each picture taken as it is (NV12)
        render::ImageId         bg_ext_tex_        = 0;
        // The background clip has put a picture in its texture: until then (or
        // with no decoder for it) the still background stands in, not black.
        bool                    bg_have_frame_     = false;

        // Idle title texture ("KESTREL" rendered at high resolution for BACKGROUND state)
        render::ImageId idle_title_tex_    = 0;   // "fpv"
        render::ImageId idle_title_os_tex_ = 0;   // "OS", drawn in the theme accent
        int    idle_title_w_   = 0;
        int    idle_title_h_   = 0;


        void draw_bg_png(float fw, float fh, const float* mvp, float warp_t = 0.0f);
        void draw_bg_video(float fw, float fh, const float* mvp, float warp_t);
        // Import a player's newest decoded picture (an NV12 DMA-BUF) as the
        // external texture `tex`, without a copy. False when there is no new one.
        bool import_drm_frame(BgVideoPlayer* player, render::ImageId tex);

        // --- Gallery (gallery.hpp has the logic, osd_gallery.cpp draws it) ---
        // Back on the live picture opens it; while it is open it draws the
        // whole frame in place of the HUD, and the key handler hands it the keys.
        Gallery gallery_;
        // The recording being played: its own decoder, its latest picture as
        // an external texture. Opened and closed on the render thread.
        std::unique_ptr<BgVideoPlayer> clip_player_;
        std::string clip_playing_;
        render::ImageId      clip_ext_tex_ = 0;
        // The live picture again, as a texture (the stats screen shows it next to the charts
        // while the video plane shows it in the strip's live tile).
        bool        live_ext_have_ = false;
        uint64_t    live_ext_pts_ = 0;
        uint32_t    live_ext_epoch_ = 0;
        bool        import_live_picture();
        bool        clip_have_frame_ = false;
        // Playback's title, timeline and key hints: shown for kPlaybackUiHoldUs
        // after a button, then faded out over kPlaybackUiFadeUs while the
        // recording plays (always shown paused or ended).
        uint64_t    playback_ui_key_us_ = 0;   // the last gallery button
        static constexpr uint64_t kPlaybackUiHoldUs = 3000000, kPlaybackUiFadeUs = 300000;
        // Thumbnails on the GPU, loaded as the strip scrolls to them.
        struct GalleryThumb {
            render::ImageId   tex = 0;
            int      w = 0, h = 0;
            uint64_t used = 0;          // frame counter, for evicting the stalest
            uint64_t next_try_us = 0;   // a thumbnail still being made is looked for again later
        };
        std::map<std::string, GalleryThumb> gallery_thumbs_;
        uint64_t gallery_frame_ = 0;
        // Where the live picture's tile is in the frame being drawn; handed to
        // the display with the frame (HudLayer::present).
        display::Tile frame_tile_;
        // Back on the live picture does not open the gallery at once: the next
        // HUD frame is drawn as usual and copied to gallery_snap_tex_ (what is on
        // the screen, whatever it is - the HUD over the picture, or the idle
        // screen), and the gallery opens after it. That copy is what shrinks into
        // the live tile with the picture, so nothing on the screen changes at the
        // moment of the press.
        std::atomic<bool> gallery_capture_{false};
        // While the gallery animates, frames are started right after a display flip lands,
        // not on a free-running timer: an OSD frame reaches the screen on the next video flip
        // (page_flip carries it), so a frame started at a fixed point in the flip cycle waits
        // a fixed time, where a timer's frames waited 0-17 ms at random - a pixel of jitter
        // on anything that moves steadily.
        std::atomic<bool> phase_lock_{false};
        std::atomic<bool> flip_tick_{false};    // a flip landed
        void on_flip();
        std::atomic<uint64_t> latency_frames_total_{0};   // pictures rendered, ever (KESTREL_PROF)
        // The renderer's latency frames on their way to osd_vars.latency_ring:
        // it pushes without a lock, the HUD drains under osd_mutex at every
        // wake of its loop and before anything reads the ring (sync_radio).
        bus::Ring<LatencyFrame, 1024> latency_in_;
        void drain_latency_frames();
        std::atomic<uint32_t> sky_exposure_us_{5000}, sky_framerate_{120};   // the camera's, for the renderer: defaults, nothing sets them
        std::atomic<uint64_t> last_flip_seen_us_{0};
        int since_flip_hist_[7] = {0, 0, 0, 0, 0, 0, 0};   // KESTREL_PROF
        render::ImageId            gallery_snap_tex_ = 0;
        bool              gallery_snap_valid_ = false;
        // The stats screen's numbers: rebuilt from the latency history and the flip log
        // about ten times a second while the stats tile or screen is showing; the
        // figures printed from it (stats_shown_) are refreshed twice a second, so the
        // text does not change - and is not re-rendered - every frame.
        StatsView stats_view_{}, stats_shown_{};
        uint64_t  stats_view_us_ = 0, stats_shown_us_ = 0;
        int       stats_view_window_ = -1;
        void stats_refresh(int window_idx, uint64_t now_us);
    void stats_prof(uint64_t now_us);
    // The history the stats screen works from, kept here and only topped up with what is new
    // (stats_history_update): up to the longest window, oldest first, from index *_head_ on.
    std::vector<StatsFrame> stats_frames_;
    std::vector<StatsFlip>  stats_flips_;
    std::vector<StatsResend> stats_resends_;              // the air radio's resends (Open8030)
    size_t   stats_frames_head_ = 0, stats_flips_head_ = 0, stats_resends_head_ = 0;
    uint64_t stats_frames_t_ = 0, stats_flips_t_ = 0, stats_resends_t_ = 0;   // the newest entry held
    void stats_history_update(uint64_t now_us);
    float stats_ymax_ = 0, stats_lmax_ = 0;     // the chart scales, eased
    float stats_dmax_ = 0;                      // the distance's (the link lane's right axis), eased
    float stats_p50_ = 0, stats_p99_ = 0;       // the p50 / p99 lines, eased
    // The stats screen's slow layer, kept as a texture (see render_gallery), and the flat
    // per-vertex-coloured program the charts are drawn with.
    struct StatsLayerKey {
        int win = -1, ymax_q = 0, lmax_q = 0, W = 0, H = 0, ox_q = 0, oy_q = 0, m_q = 0, theme_q = 0;
        uint64_t shown_us = 0;
        bool operator==(const StatsLayerKey& o) const {
            return win == o.win && ymax_q == o.ymax_q && lmax_q == o.lmax_q && W == o.W && H == o.H &&
                   ox_q == o.ox_q && oy_q == o.oy_q && m_q == o.m_q && theme_q == o.theme_q && shown_us == o.shown_us;
        }
    };
    StatsLayerKey stats_layer_key_{};
    bool   stats_layer_valid_ = false;
    render::ImageId stats_layer_tex_ = 0;
    uint64_t stats_clock_us_ = 0;               // the chart's time base (see render_gallery)
    double stats_dt_us_ = 16667.0;
    int gallery_dt_hist_[6] = {0, 0, 0, 0, 0, 0};   // KESTREL_PROF: frame intervals
        void gallery_capture_frame(int screen_w, int screen_h);
        void gallery_prewarm();
        void gallery_release();                  // closed: its textures and the stats history go
        bool render_gallery(int screen_w, int screen_h);
        void service_clip_player(const Gallery::Snapshot& s);
        render::ImageId gallery_thumb(const std::string& name, int& w, int& h, bool may_load, bool& loaded);
        void present_frame();
        std::function<void(int w, int h)> frame_hook_;   // set_frame_hook

        static const int KEY_UP = 0x101;
        static const int KEY_DOWN = 0x102;
        static const int KEY_RIGHT = 0x103;
        static const int KEY_LEFT = 0x104;

        // Menu State
        bool menu_open = false;
        // Open the menu as soon as the OSD comes up. Only --menu sets this: the
        // menu is otherwise unreachable without pressing a button on the goggle,
        // which makes it the one part of the UI that cannot be looked at from a
        // workstation. Every other screen can be captured off the DRM plane.
        bool menu_open_at_start = false;
        int menu_index = 0;
        int menu_video_mode = 0; // index into video_mode_names (or the built-in list)
        int menu_cam_flip = 0;   // camera rotation: 0 = upright, 1 = 180
        int menu_cam_focus = 0;  // Focus Mode (sky cmd 0x0E, CMD_SET_CHN_FOCUS)
        // When the video source supplies its own mode list (the AR8030 camera
        // has a different one from the ArtLynk air unit), the VIDEO tab shows
        // these labels and cycles over them instead of the built-in four.
        std::vector<std::string> video_mode_names;
        std::vector<uint16_t> video_mode_needs;  // per mode: the air features it takes
        std::atomic<uint16_t> air_feat_{0};
        // The camera rows the air unit applies (air::UnitState::cam_settings,
        // bit n for video_row n); the others are greyed.
        std::atomic<uint16_t> air_cam_{0xffff};
        bool cam_row_ok(int row) const { return row < 1 || row > 9 || (air_cam_.load() >> row & 1); }
        // VIDEO: the row the handlers number a listed index as - Max Bitrate
        // (10), Strips (12) and Max Exposure (11) right after Video Mode, the
        // last two listed only where they can be had, then the camera rows
        // 1-9; -1 past them.
        int video_row(int shown) const;
        std::atomic<bool> air_stock_{false};
        // AR8030 camera menu: the VIDEO tab shows the camera's own settings
        // instead of the kestrel-air encoder knobs, which do not apply here.
        // Bitmask of RF controls the baseband accepted (gnd::RfCap).
        // 0 = unknown/none, which leaves the RF tab as it was for other links.
        std::atomic<unsigned> rf_caps{0};
        // Raw baseband ranging reading (metres, -1 none), and its zero below.
        // The reading with the units side by side is not 0 and differs by
        // radio firmware (stock ~110, Open8030 ~70) and bandwidth (stock
        // subtracts a per-bandwidth table, ar_ldy_gnd 0x9be20): the zero is
        // taken from the reading where the aircraft is armed instead.
        uint64_t rec_started_us = 0;   // for the REC elapsed timer
        std::atomic<int> link_distance_raw{-1};
        // The distance shown (gnd::RadioState::dist_m), metres: -1 none.
        int  dist_m_ = -1;
        // Wall-clock readout, top centre. The time itself comes from the
        // pcf8563 at boot (see /etc/init.d/S36rtc); this only displays
        // whatever the system clock says.
        // 0 = off, 1 = 12-hour ("3:11 PM"), 2 = 24-hour ("15:11").
        // Two independent questions: clock_show is WHEN (0 off, 1 idle only,
        // 2 always), clock_mode is WHAT FORMAT (1 = 12H, 2 = 24H).
        // Which colour theme the HUD draws in. See hud_theme.hpp - the index
        // is into that table, and is clamped on load.
        int  hud_theme_idx = 0;
        int  menu_hud_theme = 0;
        int  clock_show = 1;
        int  menu_clock_show = 1;
        int  clock_mode = 2;
        int  menu_clock_mode = 2;
        // Which overlay is drawn over the video. One setting where there
        // were three (detail level, layout, MSP on/off) whose combinations
        // mostly did not exist: the canopy has one level, only the arena
        // panels have a fuller one. Persisted as hud_layout; the old keys
        // are migrated once at load.
        // Listed in the order the value column cycles through: OFF first,
        // since it is where a pilot who wants only their own FC's OSD lands
        // fastest, then the two overlays from plainest to fullest. The
        // ordinals are display order, not a default - see where hud_style is
        // initialised and migrated for what a fresh goggle actually starts on.
        enum HudStyle { kHudOff = 0, kHudCanopy, kHudArena, kHudArenaFull, kHudStyleCount };
        int  hud_style = kHudCanopy;
        int  menu_hud_style = kHudCanopy;
        // Betaflight's own OSD canvas, drawn whole over whichever style is on.
        bool bf_osd = true;
        bool menu_bf_osd = true;
        // Canopy headline voltage: 0 per cell, 1 whole pack. Per cell by
        // default - it is the same number on every pack size, where a pack
        // reading means nothing until you know the cell count.
        int  volt_mode = 0;
        int  menu_volt_mode = 0;
        int  menu_wifi_ap = 0;     // SYSTEM > WiFi AP (fpvos-wifi)
        // DVR > Format Card (dvr/format_card.hpp): what the firmware would
        // erase, read on first showing; armed by a first Enter for 5 s; how
        // the last format went (it restarts us), shown on the row.
        bool        format_checked_ = false, formatting_ = false;
        uint64_t    format_armed_ms_ = 0, format_armed_at_ms_ = 0;
        dvr::format_card::Result format_info_;
        std::string format_result_;
        void        format_card_enter();
        // DISPLAY > Screen Mode: 0 is Auto, then the connector's modes as
        // screen_modes() lists them. -1 until first read from screen_mode.
        int  menu_screen_mode = -1;
        std::string screen_id_;
        bool        screen_confirm_ = false;          // a mode on trial, asking
        std::string screen_confirm_mode_;             // it: "WxH@R", or "auto"
        uint64_t    screen_confirm_deadline_ms_ = 0;
        int         screen_confirm_left_s();
        // DISPLAY > Refresh Sync on trial: NUDGE or FOLLOW applied but not saved
        // until Enter; no answer in 10 s puts the kept value back (a screen
        // that blanks on it cannot be answered from).
        bool        sync_confirm_ = false;
        int         sync_confirm_prev_ = 0;          // the kept value
        uint64_t    sync_confirm_deadline_ms_ = 0;
        int         sync_confirm_left_s();
        struct ScreenModeOpt { int w, h, hz; };
        // Made only on the HUD's thread under osd_mutex (refill_screen_modes),
        // read there unlocked while drawing and under the lock by the keys.
        std::vector<ScreenModeOpt> screen_modes_;
        bool screen_modes_stale_ = true;       // made again at the next frame (a menu open asks)
        const std::vector<ScreenModeOpt>& screen_modes();
        void refill_screen_modes();
        int  screen_mode_saved();              // screen_mode's menu index
        int  screen_mode_menu();               // menu_screen_mode, read in
        std::string screen_mode_label(int idx);
        // Last sysfs read of this board's SoC temperature, cached by the top
        // status row so the canopy panel does not read thermal_zone0 again.
        float vrx_temp_cached_ = 0.0f;
        // Same reason as vrx_temp_cached_: sampled on the status row's timer
        // so the canopy panel does not re-read the ADC every frame.
        float vrx_volts_cached_ = 0.0f;
        bool     hud_wifi_on_ = false;      // wifi_ap_on(), read once a second for the HUD
        uint64_t hud_wifi_us_ = 0;
        // Arm-transition wipe: the frame line is redrawn in the new state's
        // colour from one end, and back the other way on disarm. -2 means
        // "nothing seen yet", which starts settled rather than animating.
        int      canopy_arm_prev_   = -2;
        uint64_t canopy_arm_change_ = 0;
        // When video actually arrived, so the lock animation plays on the real
        // event rather than on a timer. Zero while still acquiring.
        uint64_t lock_locked_us_ = 0;
        // Link-acquired sweep. Zero until the radio has linked at least once,
        // which is also what says "do not draw the panels yet".
        bool     canopy_link_prev_  = false;
        uint64_t canopy_link_up_    = 0;
        uint64_t canopy_link_down_  = 0;   // when CONNECTED was last left
        // Drives the STANDBY label's wipe-reveal (draw_text_wipe) - the gap
        // it sits in is fixed-size like DISARMED's, but the word itself
        // still needs its own timer since, unlike ARMED/DISARMED, nothing
        // else animates a standby toggle that happens mid-connection.
        // Settles instantly on the first frame ever seen rather than
        // sweeping in a word the goggle booted already showing.
        bool     canopy_standby_seen_   = false;
        bool     canopy_standby_prev_   = false;
        uint64_t canopy_standby_change_ = 0;
        // Top of the canopy blades, in frustum units. Written every time they
        // are drawn; the default is what they occupy at 1.0x, so the menu is
        // still placed sensibly on a frame where the canopy has not drawn.
        float canopy_top_y_ = -0.40f;

        // The one answer to "is there an aircraft", settled. Both the top strip
        // and the canopy read it, so the clock cannot disappear a beat before
        // the panels arrive.
        //
        // The timer is on the DROP. Rising must be instant, because the rebuild
        // sweep plays on it and anything drawn before that sweep is a HUD that
        // looks connected and then reassembles itself. Holding the drop gets
        // the flicker protection instead: a link that blinks never leaves
        // CONNECTED, so there is no rebuild to suppress.
        static constexpr uint64_t kLinkDropHoldUs = 1500000;   // 1.5s of silence
        uint64_t hud_raw_down_us_ = 0;
        bool     hud_connected_   = false;
        // When the current connection began, and how many frames have been
        // decoded since. The acquiring screen needs to tell "no picture yet from
        // this aircraft" from "the picture stopped".
        //
        // Counted, not timestamped: on reacquire the air unit delivers a backlog
        // of buffered frames, and one of them can leak out well before the
        // stream really starts. A single frame arriving was enough to declare
        // the video acquired, which killed the lock screen for the rest of the
        // connection - and then the picture froze anyway, because that frame was
        // the whole of the backlog. Ten frames is a fifth of a second of real
        // video at 60fps and nothing a stray frame can fake.
        static constexpr unsigned kFramesForVideo = 10;
        uint64_t hud_connected_since_us_ = 0;
        std::atomic<unsigned> frames_since_connect_{0};   // the renderer counts, the HUD resets
        int  dist_shown() const { return dist_m_; }   // metres, -1 no ranging
        // The acquiring screen runs for at least this long from the link edge,
        // the same edge the canopy's rebuild sweep fires on. Without it the
        // screen is hostage to how fast the air unit dumps its startup buffer:
        // frames captured 16.7ms apart arrive 0-4ms apart, so ten of them land
        // inside a few milliseconds and the screen is gone before it is seen.
        static constexpr uint64_t kLockMinUs  = 1200000;   // 1.2s on screen
        static constexpr uint64_t kLockFadeUs = 400000;    // last 0.4s fades
        uint64_t lock_hold_until_us_ = 0;
        render::ImageId clock_tex = 0;   // canopy icons are vectors now (vec_icons.hpp)
        // Set by the radio, read by the HUD: atomics, no lock.
        std::atomic<float> vtx_cpu_c{0.0f};   // air-side SoC, cmd 0x05 byte 1
        std::atomic<float> rsm_latency_ms{0.0f};  // AU reassembly, from Ar8030Link
        std::atomic<float> air_delay_ms{0.0f};    // air capture -> ground arrival
        std::atomic<bool>  air_delay_valid{false};
        std::atomic<bool>  vtx_temp_valid{false};
        int menu_ar_power = 500;   // mW, stock's encoding (see kArPwrLevels)
        int menu_ar_chan  = -1;   // -1 = AUTO
        bool menu_ar_hop  = true;  // channel hopping (AUTO/ACS)
        uint64_t serial_tab_us_ = 0;   // the SERIAL tab's last redraw
        uint64_t serial_rate_us_ = 0;  // its rates: the last counts, a second apart
        uint32_t serial_prev_ = 0;
        uint32_t serial_prev_bytes_ = 0;
        float    serial_bps_ = 0;               // bytes a second
        float    serial_rate_ = 0;             // frames a second
        int menu_ar_maxbr = 0;     // index into kArMaxBrVals (0 = AUTO, no cap)
        int menu_ar_maxexp = 3;    // index into kArMaxExpVals (3 = 4 ms)
        int menu_ar_strips = 1;    // index into kArStripsVals (1 = 2 strips)
        int menu_ar_maxbw = 4;     // index into kArMaxBwVals (0 = AUTO, 4 = 40 MHz)
        int menu_ar_frame = 0;     // index into kArFrameVals (0 = SHORT, mode 2)
        int menu_ar_maxmcs = 0;    // RADIO > Max MCS: 0 AUTO, n = MCS n-1 (Open8030)
        int menu_ar_steady = 0;    // RADIO > Steady Latency: 0 OFF, 1 ON (Open8030)
        int menu_ar_fw = 0;        // RADIO > VRX Radio Firmware: 0 STOCK, 1 OPEN8030
        int menu_cam_ev       = 3;  // index into kEvSteps below (3 = 0.0 EV)
        int menu_cam_sat      = 0;
        int menu_cam_contrast = 0;  // 0-15: the config packs it into a nibble
        int menu_cam_sharp    = 0;
        int menu_cam_scene    = 0;
        int menu_cam_wb       = 0;
        int menu_cam_3dnr     = 0;   // index into kDnrVals
        bool menu_scan_open = false;   // AR8030 > Channel: the channel page is open
        int  menu_scan_sel = 0;        // its cursor: 0 AUTO, r + 1 the r-th channel by frequency
        std::atomic<bool> air_standby{false};   // as REPORTED by the air unit (TLV 0x12)
        bool menu_ar_standby = false;  // the menu's value: seeded from air_standby,
                                       // toggled to force standby on/off (momentary,
                                       // never persisted - the air still overrides it)
        // Whether the VTX is ACTUALLY drawing standby power, inferred from
        // bytes 29-30 of the cmd 0x05 VTX telemetry frame rather than from
        // the TLV 0x12 standby report above - which was confirmed stuck
        // reporting ON for an entire session while this signal correctly
        // tracked a real arm/disarm transition (temperature climbing
        // 47C->64C in lockstep). See ar8030_link.cpp and
        // vtx-standby-detection-via-cmd05-bytes2930.md.
        std::atomic<bool> vtx_low_power{false};
        int  menu_brightness = 50;     // HDMI connector brightness, 0..100
        // 30 s max-hold energy per radio channel index, and when it was last set
        // (0: never). As many as gnd::ScanSample carries: at 32 the radio's 42
        // channels wrote past the end of both arrays.
        int      scan_peak_dbm[64] = {0};
        uint64_t scan_peak_ts[64]  = {0};
        int menu_fec_idx = 0; // Index into [1:1, 8:10, 8:12, 8:14, 1:2]
        int menu_ack = 0; // 0: OFF, 1: ON
        int menu_chan_auto = 1; // 1: AUTO channel hop, 0: FORCE fixed channel
        int menu_stbc = 1; // 0: OFF, 1: ON
        int menu_ldpc = 1; // 0: OFF, 1: ON
        
        // Track what has been applied/received to only send diffs
        int applied_video_mode = -1;
        int applied_wifi_chan = -1;
        int applied_tx_power_idx = -1;
        int applied_mcs = -1;
        int applied_bitrate = -1;
        int applied_fec_idx = -1;
        int applied_ack = -1;
        int applied_intrarefresh = -1;
        int applied_slice = -1;
        int applied_stbc = -1;
        int applied_ldpc = -1;
        int applied_pkt_size_idx = -1;
        int menu_pkt_repeat = 0;
        int applied_pkt_repeat = -1;
        float menu_ui_scale = 1.0f;
        float applied_ui_scale = -1.0f;
        int menu_picture_scale = 100;   // whole-picture scale, percent (100 = full)
        int menu_frame_mode = 0;        // FrameMode (frame_mode.hpp)
        int menu_refresh_sync = 0;      // display::RefreshSync's mode, for the screen in use
        int menu_sync_slice = 1;        // ... its slice: 1 or 2
        int menu_sync_margin = 0;       // ... its margin, ms: 0..4

        int menu_tab = 0; // 0: Video, 1: RF

        bool show_all_adapters = false; // Default OFF
        bool bg_video_enabled = true;   // Default ON — use background.mp4
        // Reactive HUD intensity: 0=OFF 1=SMALL 2=MEDIUM 3=EXTREME.
        // MEDIUM is the tuned default (see kBankGain/kMaxOffset in msp_osd).
        int  hud_reactivity = 2;
        bool hud_debug = false;   // HUD > Debug Info: a line of radio details under the right panel

        // A row is "modified but not committed" from the moment left/right
        // changes it until Enter applies it. Tracked as a flag per row rather
        // than by diffing against the applied value: several rows have no
        // committed twin to compare with, and a few (Overscan, MSP OSD) preview
        // live, so equality would not mean "saved" for them anyway.
        // kMenuRows is the most rows a tab may hold - it sizes menu_dirty[][] - not
// how many are on screen at once. The list scrolls, so raising this costs one
// bool per row and nothing else.
        // VIDEO, AR8030, HUD, DISPLAY, DVR, SYSTEM. DVR sits next to DISPLAY
        // because it is a thing you set; SYSTEM is last because it is a thing
        // you read.
        // INFO (6) and SERIAL (7) came later: they are the air unit's, listed after RADIO on its
        // blade (ui_side_tabs: SERIAL, then INFO), and numbered last so no other tab moves.
        static const int kMenuTabs = 8, kMenuRows = 24;
        static const int kTabDvr = 4, kTabSystem = 5, kTabAirInfo = 6, kTabAirSerial = 7;
        // DISPLAY rows, in the order they are listed. The tab is dispatched
        // by row in half a dozen places, so the order lives here once and the
        // dispatchers name the row rather than count to it. Refresh Sync and
        // Sync Margin are listed only where the frame length can be set, Sync
        // Margin only while Refresh Sync is NUDGE or FOLLOW, and Sync To not at
        // all (hidden, its setting still read): the rows after them close up,
        // so a listed index goes through disp_row(). Screen Mode, before them,
        // never moves (its pending choice is menu_dirty[3][kDispScreenMode]).
        enum DisplayRow {
            kDispTheme = 0, kDispBgVideo, kDispPicture, kDispUiScale, kDispBrightness,
            kDispHdrAdvanced, kDispScreenMode, kDispRefreshSync, kDispSyncMargin, kDispFrameMode,
            kDispSyncSlice
        };
        // SYSTEM rows, headers included, in the order they are listed.
        enum SysRow {
            kSysBuild = 0, kSysVersion, kSysDecoder, kSysWifi,
            kSysHdrZone, kSysLicence, kSysArea, kSysCountry, kSysTimezone
        };
        int  menu_licence_ = 0;   // SYSTEM > Amateur Licence's mirror (zones::licensed())
        int  arm_seen_ = -1;      // the arm state last frame (frame_timers closes the menu on arming)
        // One step on the Area, Country or Timezone row: the zone it lands on.
        // Timezone is only listed for a country with more than one zone.
        int  menu_step_zone(int row, int dir);
        // HUD rows, in the order they are listed.
        enum HudRow {
            kHudRowStyle = 0, kHudRowBfOsd, kHudRowVoltage, kHudRowClock, kHudRowDynamic,
            kHudRowDebug
        };
        // A row's help line, which for a few rows depends on the value it is
        // showing; everything else gets its MenuItem::help back.
        const char* menu_help_text(int tab, int i, const char* fallback);
        // The HUD tab lists Canopy Voltage only with the canopy up, which moves the rows after it:
        // the HudRow a listed row is, whatever is shown above it.
        int hud_row(int shown) const;
        // DISPLAY: the DisplayRow a listed index is, rows not listed skipped.
        int disp_row(int shown) const;
        // Amber line below a row's help, shown whatever its value; nullptr for none.
        const char* menu_help_warning(int tab, int i);
        bool menu_dirty[kMenuTabs][kMenuRows] = {};
        void menu_mark_dirty(bool on);
        // The committed value of a dirty row, as text, taken the moment the
        // arrows first move it. The value column shows "Pending: from -> to"
        // from this, and a row walked back to this text is clean again -
        // text equality is the one test that needs no per-row table.
        char menu_pending_from[kMenuTabs][kMenuRows][32] = {};
        void menu_plain_value(int tab, int i, char* out, size_t cap);
        // "menu: HUD > HUD Style: ARENA" in the log: a row opened to change it
        // (how "edit"), and what was then applied ("live", "enter").
        void menu_log_applied(const char* how);
        bool menu_show_all_adapters = false;
        bool menu_bg_video = true;
        int  menu_hud_reactivity = 2;
        bool menu_hud_debug = false;
        // DVR Source row: 0 FPV, 1 SCREEN at 30 captures/s, 2 SCREEN at 60.
        int  menu_dvr_source = 0;
        int  dvr_source_now() const;
        // What the menu asked of the link's parts, shown until each has
        // carried it out (hud/link_view.hpp).
        hud::Asked asked_;
        hud::LinkView link_view() const;   // the parts' settings, with what was asked on top
        // A command code nothing takes (the old air link's: 0x0E, 0x101, 0x104,
        // 0x107): logged as unknown, as it always was.
        static void unknown_command(int code, int value);
        // The link as this frame shows it: taken under osd_mutex at the start
        // of each frame, read by the drawing code and, under the same mutex,
        // by key handling's menu text.
        std::shared_ptr<hud::LinkView> link_;
        // What the screens draw their panels, text, arcs and bars with
        // (render/canvas.hpp): the platform's (CanvasGles on a GPU).
        std::unique_ptr<render::Canvas> canvas_;
        int drift_request = 0;

        std::string decoder_name;
        std::shared_ptr<DVR> dvr;
        bool dvr_screen = false;
        pthread_t tid_dvr_menu;
        bool tid_dvr_menu_active = false;
        render::ImageId live_ext_tex_ = 0;
        render::ImageId live_video_tex_ = 0;

        // A row's possible values, discovered rather than declared.
        //
        // Returns how many it found and which one is current. A row whose
        // handler cycles - almost all of them - is enumerated by stepping it
        // and reading the value back; one that does not cycle inside the cap
        // (a number with a range, say) reports 0 and is shown as a bare value.
        static constexpr int kMenuOptMax = 12;
        static constexpr int kMenuOptLen = 28;
        int  menu_options(int tab, int index, char out[kMenuOptMax][kMenuOptLen], int* cur);
        // The values either side of the current one, for a row that is a range
        // rather than a set. Fills 2*span+1 entries with the centre at [span];
        // an entry past the end of the range is left empty.
        static constexpr int kMenuLadderSpan = 4;   // 9 entries; the draw shows what fits
        int  menu_range_ladder(int tab, int index, int span,
                               char out[kMenuOptMax][kMenuOptLen]);
        // The ladder for whichever row is selected, worked out on the thread
        // that owns the menu state and read by the one that draws it. Both
        // ways of finding a row's values step the row's own handler, which is
        // a write - doing that from the render thread raced every keypress.
        void menu_refresh_options();
        char menu_lad_[kMenuOptMax][kMenuOptLen] = {};
        int  menu_lad_n_   = 0;
        int  menu_lad_cur_ = 0;    // where the value in force sits in menu_lad_
        int  menu_lad_tab_ = -1;
        int  menu_lad_idx_ = -1;
        // One press on a row that holds a set. Wraps for the arrows; stops at
        // the ends while a row is being enumerated (menu_stepping_).
        int  menu_step(int v, int dir, int n) const;
        // Enumeration steps real handlers, so it is done once per row rather
        // than per frame, and the answer is kept until the selection moves.
        int  menu_opt_tab_ = -1, menu_opt_idx_ = -1, menu_opt_n_ = 0, menu_opt_cur_ = 0;
        char menu_opt_[kMenuOptMax][kMenuOptLen] = {};
        // Which column has focus: 0 section, 1 setting, 2 value.
        int  menu_focus = 1;
        // The row being changed (focus 2): its tab and label. OK and the
        // arrows act on it only while it is still the row under the cursor
        // (menu_edit_row_intact).
        int  menu_edit_tab_ = -1;
        std::string menu_edit_label_;
        bool menu_edit_row_intact() const;
        // True while a walk is enumerating a row's values by stepping its own
        // handler. The step has to move the menu's mirror of the setting -
        // that is what the walk reads back - but it must not apply it to the
        // HUD or write it to the file on the way past.
        bool menu_stepping_ = false;
        // Rows that take effect the moment the arrows move them. Those are
        // written to the settings file there and then, and never wear the
        // pending mark: there is nothing left for Enter to apply.
        static bool menu_row_applies_live(int tab, int index);
        // The menu has never been opened this boot, so the first open picks
        // where to start rather than resuming a selection nobody made.
        bool menu_first_open_ = true;

        // One left/right step on a row, without a keypress. See the definition.
        int  menu_first_selectable_row(int tab);
        void menu_apply_change(int tab, int index, int dir);
        // What one row's value reads as. See the definition.
        void menu_value_text(int tab, int i, char* out, size_t cap);

        // ---- the canopy is the settings (render_frame, hud_menu.cpp) ----
        // OK on the live picture wakes the canopy: its two blades lift, and
        // Left/Right picks one. The left blade shows the goggle and the pilot,
        // so it holds the goggle's settings (HUD, DISPLAY, DVR, SYSTEM); the
        // right one shows the link and the air unit, so it holds the air
        // unit's (VIDEO, RADIO). OK unfolds the picked blade up its side of the
        // screen into its settings, the middle of the picture left clear; on
        // RADIO > Channel, OK unfolds it the other way, along the bottom, into
        // the channel band (menu_scan_open). Back folds one step at a time.
        // Back on the live picture still pulls out to the gallery.
        int      ui_stage_ = 0;         // 0 a blade picked, 1 its settings unfolded
        int      ui_side_ = 1;          // 0 the left blade (goggle), 1 the right (air unit)
        int      ui_side_tab_[2] = { 2, 1 };   // each side's section last open (menu_tab)
        // The motion, each eased toward its target the gallery's way.
        float    ui_wake_ = 0.0f;       // the canopy awake (menu_open)
        float    ui_lift_[2] = { 0.0f, 0.0f };   // a blade lifted (picked) or lowered (not)
        float    ui_open_[2] = { 0.0f, 0.0f };   // a blade unfolded into its settings
        float    ui_band_ = 0.0f;       // the right blade unfolded into the channel band
        uint64_t menu_anim_us_ = 0;
        // The sections each side holds, in order (menu_tab numbers); VIDEO is
        // left out while nothing is linked.
        std::vector<int> ui_side_tabs(int side) const;
        // The picture's rectangle and where each blade is, in pixels.
        void ui_blade_rect(int side, int W, int H, float fh, float& x, float& y, float& w, float& h);
        // (draw_canopy_ui and draw_canopy_pick are with draw_canopy_hud: they
        // take its CanopyIn for the panel matrices.)
        void draw_side_panel(int side, float x, float y, float w, float h, float u);

        // ---- the channel page (osd_channel.cpp) ----
        int  chan_order_[64] = {};      // radio channel indices by frequency
        int  chan_order_n_ = 0;
        void chan_open();
        void chan_close();
        void chan_key(int key);
        int  chan_quietest() const;
        int  chan_bw_mhz() const;
        int  chan_tx_mw() const;
        // Draws the band in the rectangle (pixels); true while something on it
        // is still gliding.
        bool draw_channel_band(int W, int H, float band_x, float band_y, float band_w, float band_hp, float t);
        // Its motion (eased toward where things belong) and the numbers it
        // prints, taken twice a second rather than at every scan update.
        float    chan_cursor_x_ = -1.0f, chan_link_x_ = -1.0f;   // slots; -1: place at once
        float    chan_bar_[64] = {}, chan_peak_[64] = {};        // dBm, by radio channel index
        bool     chan_bars_set_ = false;
        uint64_t chan_anim_us_ = 0, chan_shown_us_ = 0;
        uint64_t chan_auto_refused_us_ = 0;   // OK on AUTO where it is not offered: its line says why, 3 s
        int      chan_shown_now_[64] = {}, chan_shown_peak_[64] = {};
        int      chan_shown_snr_ = 0, chan_shown_mcs_ = 0;
        bool     chan_shown_linked_ = false;
        // SYSTEM > Time Zone (zones.hpp): the menu's mirror of the zone in force
        // (-1 not set), walked by its Area / Country / Timezone rows, and the
        // zones::generation() it was last seeded from.
        int      menu_zone_ = -1;
        uint32_t menu_zone_gen_ = 0;

        // ---- pixel-space pages (osd_screen.cpp) ----
        int   px_W_ = 0, px_H_ = 0;
        float px_cx_ = 0, px_cy_ = 0, px_sc_ = 1, px_a_ = 1;
        float px_mvp_[16] = {};
        void  px_begin(int W, int H, float cx, float cy, float sc, float alpha);
        std::vector<float> px_batch_;   // gathered flat shapes (px_flush draws them)
        void  px_flush();
        void  px_vertex(float x, float y, const float* c, float a);
        void  px_fill(float x, float y, float w, float h, const float* c, float a);
        void  px_poly(const float* p, int n, const float* c, float a);
        void  px_quad(const float* p, const float* c, float a);
        void  px_frame(float x, float y, float w, float h, float t, const float* c, float a);
        void  px_hatch(float x, float y, float w, float h, const float* c, float a, float pitch, float stripe);
        float px_text(const char* s, float x, float baseline, float font_px, int align, const float* c, float a);
        float px_text_w(const char* s, float font_px);
        float px_pill(const char* s, float x, float y, float font_px, const float* bg, const float* fg, float a, bool outline);
        bool  px_live(float x, float y, float w, float h);
    public:
        int refresh_frequency_ms;
        // The renderer asks every frame: no lock.
        int stats_period_ms() const override { return refresh_frequency_ms; }
        uint32_t get_sky_exposure_us() override {
            return sky_exposure_us_.load(std::memory_order_relaxed);
        }
        uint32_t get_sky_framerate() override {
            return sky_framerate_.load(std::memory_order_relaxed);
        }

    private:
        void init_images();
        void render_frame();
        render::ImageId load_image_from_png(const unsigned char* png, unsigned int length);
        render::ImageId load_image_from_file(const char* filename);
        void draw_debug_chart();
        // --- SIGNAL LOCK (hud_lock.cpp) ---
        // The screen between "something is out there" and "there is a picture".
        // The acquiring-video lock. Binding reuses it whole rather than
        // growing a second animation that means almost the same thing: rings
        // contracting onto an aircraft is exactly what a bind is, and the two
        // must not disagree about what the theme's colours mean.
        //
        // Defaults reproduce the full-screen acquiring behaviour exactly.
        //   scale      shrinks the whole figure (everything is sized off U)
        //   cy_frac    centre height, as a fraction of frustum_h
        //   text_scale captions only, so a small figure can keep big type
        //   head/sub   override the captions; null keeps ACQUIRING VIDEO and
        //              the live pipeline stage from lock_stage()
        //   clock      an alternative start timestamp, so a second user of
        //              this animation does not share lock_since_us_ and
        //              restart the acquiring screen mid-pass
        void draw_signal_lock(float frustum_w, float frustum_h, const float* mvp,
                              float fade, float scale = 1.0f, float cy_frac = 0.09f,
                              float text_scale = 1.0f,
                              const char* head_override = nullptr,
                              const char* sub_override = nullptr,
                              uint64_t* clock = nullptr,
                              uint64_t locked_at = 0);
        int  lock_stage(char* out, size_t out_len);
        uint64_t lock_since_us_     = 0;   // when this pass of the lock began
        uint64_t handshake_sent_us_ = 0;   // set by the source, read by lock_stage


        // --- CANOPY HUD (hud_canopy.cpp) ---
        // Everything the canopy layout draws, gathered at the one point in
        // render_frame() that already holds the snapshots. Passing a struct keeps
        // the signature from growing a parameter every time a slot is added,
        // and keeps the layout code out of the 5000-line render_frame().
        struct CanopyIn {
            float frustum_w = 0.0f, frustum_h = 0.0f;
            float overscan  = 0.0f;
            float s         = 1.0f;      // ui_scale
            // The panel matrices are handed over unfinished: the canopy needs
            // to insert its own in-plane slant between the model rotation and
            // the view, and the pivot for that slant is a canopy coordinate.
            const float* model_l = nullptr;
            const float* model_r = nullptr;
            const float* vp      = nullptr;   // view * projection
            bool  video_active = false;
            bool  link_up      = false;
            // Right panel
            float range_norm = 0.0f;     // range left to fade, 0..1 (the VIDEO track, twelve blocks)
            float quality   = 0.0f;      // composite link health, 0..1
            int   link_warn = 0;         // 0 fine, 1 amber, 2 red (off the VIDEO track's blocks)
            int   dist_m    = -1;        // -1 = no ranging
            float snr_db    = -1.0f;     // the video link's SNR, dB (-1: no link)
            char  vtx_pwr[24] = {0};
            char  vrx_pwr[24] = {0};     // our radio's TX power as it pushes it, "" = not known
            int   link_freq_mhz = 0;     // 0 = unknown, do not show
            int   link_bw_idx = -1;      // the video link's bandwidth gear (gnd::bw_step_label), -1 = unknown
            // What the video link is actually delivering: the mode it is
            // sending, the bitrate it is spending on it, and how long a frame
            // takes to arrive. The three belong on one line because no one of
            // them means much alone - 18 Mbps is generous at 1080p60 and thin
            // at 1080p120.
            int   v_w = 0, v_h = 0, fps = 0;
            float mbps = 0.0f;
            float link_mbps = 0.0f;      // what the link carries at its MCS now (the radio's figure), 0 = unknown
            char  debug[64] = {0};       // HUD > Debug Info's line under the right panel, "" = none
            bool  have_lat = false;
            float lat_med_ms = 0.0f;
            // SPLIT: each slice of the picture on its own, top to bottom (0: none).
            int   lat_nslices = 0;
            float lat_slice_ms[kLatSlices] = {0, 0, 0, 0};
            bool  vtx_temp_valid = false;
            float vtx_temp_c = 0.0f;
            // Live standby, not the TLV enable/disable setting - see
            // OSD::vtx_low_power and ar8030-power-and-standby-verified.md.
            bool  vtx_low_power = false;
            bool  rec_active = false;
            unsigned rec_secs = 0;
            // REC gone off by itself, the card no longer taking it: what the
            // DVR said ("CARD FULL"), shown where REC was for 5 s; "" else.
            char  rec_failed[16] = {0};
            // Left panel
            BfTelem bf;
            float vrx_temp_c = 0.0f;
            // This goggle's own supply voltage, read from the baseband's ADC
            // (see Ar8030Link::vrx_volts). 0 = not read yet / unavailable,
            // and the HUD draws nothing rather than inventing a reading.
            float vrx_volts  = 0.0f;
            // The goggle's WiFi access point is on (SYSTEM > WiFi AP).
            bool  wifi_on    = false;
        };
        void draw_canopy_hud(const CanopyIn& in);
        // The canopy awake (osd.cpp), and the blades' names while a side is
        // being picked (hud_canopy.cpp), in the blades' own slanted space.
        // Only the frame and the panel matrices of `geo` are read.
        void draw_canopy_ui(int W, int H, const CanopyIn& geo);
        void draw_canopy_pick(const CanopyIn& in, float pick);

        // ---- a frame (render_frame, osd_frame.cpp) ----
        // What a frame takes once and works out once - the GL and the
        // matrices, the snapshot under osd_mutex, the video's state, the
        // link as measured - and each screen then draws from, in render_frame's
        // order. The screens are in their own files, each naming the fields
        // it reads at its top.
        struct Frame {
            int screen_w = 0, screen_h = 0;
            math::Mat4 projection, view, tmp;    // tmp: view * projection
            math::Mat4 model_c, model_l, model_r, model_hud_c;
            math::Mat4 mvp_c, mvp_l, mvp_r, mvp_hud_c;
            math::Mat4 model_top, mvp_top;
            float hud_dx = 0.0f, hud_dy = 0.0f, hud_bank = 0.0f;   // the HUD's motion (Dynamic HUD)
            float frustum_h = 0.0f, frustum_w = 0.0f;
            float hud_margin_l = 0.02f, hud_margin_r = 0.02f, hud_margin_b = 0.02f;
            // The snapshot (frame_take).
            LatencyFrame last_latency_frame{};
            bool  have_latency_frame = false;
            float latency_median_ms = 0.0f;
            bool  have_latency_median = false;
            float latency_slice_ms[kLatSlices] = {};
            int   latency_nslices = 0;
            packets_stats link_stats_copy;
            uint32_t v_width = 0, v_height = 0;
            int   cur_fps = 0;
            float v_bw = 0.0f, lat_avg = 0.0f, lat_max = 0.0f;
            bool  slices_received = false;
            bool  menu_moving = false, is_menu_open = false;
            float s = 1.0f;                      // the UI scale
            bool  hud_panels = false, canopy_on = false, legacy_panels = false;
            bool  hud_verbose = false, legacy_verbose = false;
            CanopyIn canopy;                     // filled as the frame goes, drawn by draw_canopy
            gnd::LinkStats link_copy;
            // The palette.
            float cyan_r = 0.0f, cyan_g = 0.9f, cyan_b = 1.0f;
            float neon_r = 0.7f, neon_g = 1.0f, neon_b = 0.0f;
            float bg_r = 0.005f, bg_g = 0.015f, bg_b = 0.04f;   // deep midnight blue
            // The video (frame_video) and the lock (draw_backdrop).
            uint64_t now_us = 0;
            VideoState vs = VideoState::BACKGROUND, prev_vs = VideoState::BACKGROUND;
            float trans_t = 0.0f;
            bool  video_active = false;
            bool  seen_this_link = false, acquiring = false, holding = false, lock_screen = false;
            float lock_fade = 0.0f;
            // The arena panels' layout (draw_arena_left).
            float panel_width = 0.0f, panel_height = 0.0f, slant_offset_x = 0.0f, slant_offset_y = 0.0f;
            float bx = 0.0f, rx = 0.0f, bg_bx = 0.0f, bg_rx = 0.0f, panel_bottom_y = 0.0f;
            float content_margin = 0.0f, bar_h_offset = 0.0f;
            // The link as measured (measure_link): the arena's and the canopy's.
            bool  connected = false;             // a good status in the last 3 s
            float dim = 1.0f;
            int   rung = -1;                     // the MCS rung (kMcsLadder), -1 none
            float snr_norm = 0.0f, quality = 0.0f;
        };
        bool frame_begin(Frame& f);              // false: the gallery drew this frame
        void frame_take(Frame& f);
        void frame_timers();                     // the trials' reverts and the time zone, every frame
        void frame_video(Frame& f);
        void frame_wings(Frame& f);              // the side panels' matrices
        void measure_link(Frame& f);
        void air_pwr_text(const gnd::LinkStats& link, char* out, size_t n) const;
        void draw_backdrop(Frame& f);            // hud_idle.cpp
        void draw_idle_title(Frame& f);
        void draw_shade(Frame& f);               // hud_strip.cpp
        void draw_top_strip(Frame& f);
        void draw_link_edge(Frame& f);
        void draw_bind(Frame& f);                // hud_lock.cpp
        void draw_arena_left(Frame& f);          // hud_arena.cpp
        void draw_arena_right(Frame& f);
        void draw_canopy(Frame& f);              // hud_canopy.cpp
        void draw_betaflight(Frame& f);          // hud_betaflight.cpp
        void draw_menu(Frame& f);                // hud_menu.cpp
        // Video link warning level for this frame: 0 fine, 1 amber, 2 red.
        // Worse levels land at once; a better one has to hold before the
        // warning steps down, so a link hovering on a threshold does not
        // flash the screen edge.
        float link_edge_a_     = 0.0f;   // edge glow intensity, faded 0..1
        int   link_edge_shown_ = 0;      // level whose colour the glow wears
        uint64_t link_edge_ts_ = 0;      // last glow update, for the fade
        // Repaint at ~30 Hz while the link is up or the glow is moving: frames
        // normally drive the repaint, and a stall - the thing being warned
        // about - is exactly when they stop.
        bool  link_watch_      = false;
        bool  link_edge_on_    = true;   // settings: link_edge_warning
        uint64_t loop_start_us_ = 0;     // when the last frame began, for pacing
        // Render pacing (see OSD::run). An animation asked for another frame
        // (drawn 16.7 ms after the last one started, not at once); new MSP
        // data arrived (redrawn only if what the HUD shows from it changed).
        bool     anim_pending_ = false;
        uint32_t last_msp_ver_ = 0;
        std::atomic<uint64_t> last_msp_signal_us_{0};
        uint64_t next_render_interval_us();

        void run();

    public:
        // The parts it shows and sets (hud/parts.hpp), watched from the start,
        // before the radio runs; the display, what it draws with (its canvas)
        // and the layer its frames go to on it, made by the platform.
        OSD(const hud::Parts& parts, Settings& settings, std::shared_ptr<display::Display> dev_,
            std::unique_ptr<render::Canvas> canvas, std::unique_ptr<display::HudLayer> layer,
            int refresh_frequency_ms_, std::atomic<bool>* signal_stop, bool console_stats_ = false);
        ~OSD();
        void signal_render(prof::Wake why = prof::kWakeOther) override;

        void update_stats(int current_framerate, latency_stats stats) override;
        void add_latency_frame(LatencyFrame frame) override;
        void set_video_resolution(int w, int h) override;
          void set_slices_received(bool val);
          bool get_slices_received();
        // The Back button (and Esc on a keyboard). On the live picture it opens
        // the gallery; with the menu open it closes it, as M does.
        static constexpr int kKeyBack = 0x105;
        void handle_key(int key);
        void set_ui_scale(float v);
        void set_decoder_name(std::string name) { decoder_name = name; }
        // The screen in use (display/screen_id.h, "" if none) and the settings
        // key its mode is kept under: screen_mode_<ID>, or screen_mode.
        void set_screen(const std::string& id) { screen_id_ = id; }
        // This run's mode is on trial (screen_mode_try, main.cpp): the menu
        // opens on DISPLAY > Screen Mode to ask whether to keep it. Enter keeps
        // it; otherwise, or unseen on a black screen, it is dropped and the
        // app restarts in the screen's kept mode.
        void begin_screen_mode_confirm(const std::string& mode);
        std::string get_decoder_name() const { return decoder_name; }
        // The air unit takes the Max Bandwidth cap (kestrel-air with the feature).
        std::atomic<bool> air_max_bw_{false};
        // RF tab contents for the AR8030, built from rf_caps: {label, field id}.
        std::vector<std::pair<const char*, int>> ar_rf_items() const;

        // One source of truth for the menu rows. Both the renderer and the
        // key handler use this, so a row cannot be added or removed without the
        // navigation bounds following - removing the Reboot row previously left
        // a hardcoded count of 7 against 6 rows, letting the cursor land on a
        // row that did not exist.
        // help is what the setting does, in a line. Several of these are not
        // self-evident from a two-word label - "Voltage: PER CELL" does not say
        // why you would want that - and the menu has the room to say so.
        // Empty is fine; the line is simply not drawn.
        // type: 0 a reading, 1 a setting, 2 a section header, 3 an action.
        // An action has no values to walk; the value column shows what Enter
        // does (hint) and, for one that runs a while, how it is going.
        // type: 0 a reading, 1 a setting, 2 a group's name, 3 an action, 4 a reading
        // whose label is the text, wrapped at ", " over as many lines as it takes.
        // off: a setting the linked air unit does not take (the stock air app),
        // listed as a reading and drawn dimmed; its help says why.
        struct MenuItem { const char* label; int type; const char* help = nullptr;
                          const char* hint = nullptr; bool off = false; };
        void menu_action_status(int tab, int i, char* hint, size_t hcap,
                                char* status, size_t scap);
        std::vector<MenuItem> menu_items(int tab) const;
        static const char* menu_intern(const std::string& s);
        void set_video_mode_names(const std::vector<std::string>& n, int current,
                                  const std::vector<uint16_t>& needs = {}) {
            video_mode_names = n;
            video_mode_needs = needs;
            if (current >= 0 && current < (int)n.size()) menu_video_mode = current;
        }
        bool video_mode_ok(int i) const {
            return i < 0 || i >= (int)video_mode_needs.size() || (video_mode_needs[i] & ~air_feat_.load()) == 0;
        }
        int get_drift_request() { int d = drift_request; drift_request = 0; return d; }
    private:
        // ---- the parts, as the HUD reads them (watched in the constructor) ----
        // Our radio, the air unit's radio, its encoder, the unit itself and its
        // camera wake the HUD when they change; our radio's scans and video
        // rate each wake it; the air radio's resends, every one, wake no one
        // (the stats screen's history, kept here); the timing is read when drawn.
        hud::Parts parts_;
        // The FC serial bridge: the radio thread only queues it, and drain_msp()
        // parses it here on the OSD thread - OSD work on the latency-critical
        // video path was both wrong by design and correlated with a SIGSEGV in
        // the renderer's frame queue. A chunk can start the FC's screen over
        // (a new link, another aircraft), in order with the bytes. Woken when
        // it goes from empty to not; taken at most every 33 ms.
        void drain_msp();
        uint64_t radio_drops_seen_ = 0;               // what the queues dropped, as last logged
        // The versions sync_radio() last took, and the last ones drawn: the
        // link's parts (our radio, the air radio, the encoder) or the air
        // unit's (the unit, its camera) have something to show when they differ.
        uint32_t radio_synced_ = 0, air_synced_ = 0, radio_drawn_ = 0, air_drawn_ = 0;
        bool radio_changed() const { return radio_synced_ != radio_drawn_ || air_synced_ != air_drawn_; }
        // The air unit and its radio as the frame shows them (under
        // osd_mutex), and the reports the menu has followed (follow_air).
        air::UnitState air_unit_;
        air::RadioState air_radio_;
        air::CameraState camera_seen_;
        uint32_t standby_seen_n_ = 0, levels_seen_n_ = 0;
        void follow_air(const air::CameraState& c);
        std::deque<StatsResend> resend_hist_;        // the last 16384, oldest first
        // TX power as the radios last said it, dBm (0: not known): kept by
        // sync_radio(), read without a lock (the renderer's samples too).
        std::atomic<int> air_live_dbm_{0}, gnd_live_dbm_{0};
        int air_live_dbm() const { return air_live_dbm_.load(std::memory_order_relaxed); }
        int gnd_live_dbm() const { return gnd_live_dbm_.load(std::memory_order_relaxed); }
        // Under osd_mutex: what the radio published since - its link and air
        // unit figures into osd_vars and the fields the HUD draws, the air
        // unit's settings into the menu - and the renderer's latency samples,
        // before anything reads them.
        void sync_radio();
        // The camera rows from the config restored and pushed, or as the air
        // unit reports it: raw wire values in, the index lookups into the
        // label tables in the .cpp, where those tables live. Under osd_mutex.
        void apply_camera_config(int ev_x10, int sat, int contrast, int sharp,
                                 int scene, int awb, int angle,
                                 int dnr3d, int focus);
        void apply_camera_dnr(int dnr3d);
        void apply_chan_scan(const gnd::ScanSample& sc);
        void log_csv_row();                        // a link CSV row, each second (under osd_mutex)
        void* csv_fp = nullptr;                    // FILE* for /tmp/kestrel-adapt.csv (lazy-opened)
    public:
        bool video_decoding();   // decoded FPV frame within the last 1.5s
    public:
        void set_dvr(std::shared_ptr<DVR> dvr_, bool dvr_screen_);
        bool is_dvr_screen_enabled() { return dvr_screen; }

        void stop();
        // Called by renderer thread every time a FPV frame is successfully flipped.
        void notify_video_frame(bool is_keyframe = false) override;

        static void* run_thread(void* arg) {
            static_cast<OSD*>(arg)->run();
            return nullptr;
        }

        // Frames drawn off the goggle (tools/test/hud_shot), on a thread that
        // is not run()'s: the GL run() sets up, one frame as run() draws it,
        // and each frame's pixels looked at before it goes to the display (the
        // hook reads them, glReadPixels).
        void test_init();
        void test_frame();
        void set_frame_hook(std::function<void(int w, int h)> fn) { frame_hook_ = std::move(fn); }

    
};

#endif

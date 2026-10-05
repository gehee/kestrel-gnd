#ifndef OSD_H
#define OSD_H

#include <vector>
#include <mutex>
#include <memory>
#include <map>

#include "drm.hpp"
#include "icons/icons.h"
#include "utils/scheduling_helper.hpp"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include "utils/prof.hpp"
#include <functional>
#include "common.hpp"
#include "utils/math_utils.hpp"
#include "msp_osd.hpp"
#include "bg_video.hpp"
#include "gallery.hpp"
#include "gallery_stats.hpp"

struct DecodedUnit;
class DVR;

extern volatile bool signal_stop;

// Kick off background-asset decoding (background.png + idle "KESTREL" title) on
// a worker thread so the heavy Cairo/PNG work overlaps DRM/EGL driver bring-up.
// Call once, early in main(), before constructing DrmDevice. init_gl_buffers()
// consumes the results (and falls back to a synchronous load if not ready).
void osd_prefetch_assets_async();

typedef struct png_closure
{
	unsigned char * iter;
	unsigned int bytes_left;
} png_closure_t;



typedef struct {
	float min;
	float max;
	float avg;
    std::vector<float> values;
} stats;

typedef struct  {
	stats proc_latency;
	stats decoding_latency;
	stats display_latency;
	stats capture_latency;
    stats tx_latency;
	stats total_latency;
	stats frame_pace;
	stats reassemble_latency;
    float proc_latency_worst, decoding_latency_worst, display_latency_worst, tx_latency_worst, capture_latency_worst, reassemble_latency_worst;
} latency_stats;

typedef struct {
    float capture_ms;
    float processing_ms;
    float net_ms;
    float reassemble_ms;
    float dec_ms;
    float disp_ms;
    float pace_ms;    // Time since last frame (frame interval)
    float video_mbps; // Video bitrate at this frame
    float rf_mbps;    // RF link bitrate (data + parity) at this frame
    // For the stats screen:
    uint64_t t_us = 0;   // when this entry was made (CLOCK_MONOTONIC)
    uint32_t lost = 0;   // link packets lost so far
    int8_t   snr = 0;    // link SNR, dB
    uint8_t  mcs = 0;    // receive MCS
    uint8_t  key = 0;    // this picture was a keyframe
} LatencyFrame;

#include "utils/latency_ring.hpp"

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
    float capture_latency_avg, capture_latency_min, capture_latency_max;
    std::vector<float> capture_latency_values;
    float reassemble_latency_avg, reassemble_latency_min, reassemble_latency_max;
    std::vector<float> reassemble_latency_values;
    float proc_latency_worst, decoding_latency_worst, display_latency_worst, tx_latency_worst, capture_latency_worst, reassemble_latency_worst;
	float video_bandwidth;
	uint32_t video_width;
	uint32_t video_height;
    uint32_t sky_exposure_us;
    uint32_t sky_framerate;

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
    LatencyRing latency_ring;               // see utils/latency_ring.hpp
    uint64_t latency_history_last_us = 0;   // last real/gap append (graph gap-filler)
    artosyn_stats artosyn;
    adapt_stats adapt;        // air-side adaptation decisions (RF telemetry)
    chan_scan_info chan_scan{}; // per-channel spectrum scan (channel-scan screen)
};




// Open the settings menu at startup (--menu). See the member for why.
void osd_set_menu_at_start(bool on);
void osd_set_menu_start_tab(int t);

class OSD {
    private:
        std::shared_ptr<DrmDevice> dev;

        // State
        struct osd_vars osd_vars = {0};
        pthread_mutex_t osd_mutex;
        bool console_stats = false;
        volatile bool* signal_stop;
        pthread_cond_t osd_cond;
        bool render_requested = false;
        // OpenGL resources
        EGLDisplay display;
        EGLContext context;
        EGLConfig config;
        
        struct GlBuffer {
            struct gbm_bo *bo;
            EGLSurface surface;
            uint32_t fb_id;
        };
        std::vector<GlBuffer> gl_buffers;
        int current_gl_buffer = 0;

        GLuint shader_program;
        GLuint vbo;

        // Cached uniform/attribute locations for shader_program. Looked up once
        // in init_shaders() instead of via glGet*Location() on every draw call —
        // the OSD recomposites the whole HUD per frame, so those per-draw string
        // lookups were a measurable chunk of the render hot path.
        GLint u_mvp_ = -1, u_is_text_ = -1, u_use_shading_ = -1,
              u_color_ = -1, u_alpha_ = -1, u_tex_ = -1;
        GLint a_pos_ = -1, a_uv_ = -1, a_alpha_factor_ = -1;
        
        // Textures
        GLuint fps_tex, lat_tex, net_tex, logo_tex, bg_tex;
        std::map<int, GLuint> rssi_texs;
        std::map<int, GLuint> link_texs;
        
        struct CachedText {
            GLuint tex;
            int w, h;
            float text_w;
            std::string text;
            // Touched on every hit so eviction can pick the genuinely coldest
            // entry. The old code evicted text_cache.begin() - the alphabetically
            // first key - which threw away hot short strings ("0", "1", ...) and
            // re-rendered them through cairo on the very next frame.
            uint64_t last_used = 0;
        };
        std::map<std::string, CachedText> text_cache;
        uint64_t text_cache_clock = 0;
        MspOsd msp_osd;
        GLuint char_tex_cache[1024] = {0};

        struct gbm_bo *locked_bo = nullptr;
        std::vector<struct gbm_bo*> bo_release_queue;
        void release_unused_bos(struct gbm_surface* gs);
        uint64_t last_render_ts = 0;
        float rotation_y = 0.0f;
        float wrap_depth = 3.2f;

        // --- Background video + warp-zoom transition ---
        enum class VideoState { BACKGROUND, TRANSITION, LIVE };
        VideoState video_state_          = VideoState::BACKGROUND;
        uint64_t   last_fpv_frame_us_    = 0;   // last frame notify timestamp
        uint64_t   transition_start_us_  = 0;   // when warp zoom started
        uint64_t   last_bg_signal_us_    = 0;   // throttles idle bg-video redraws
        // The fpvOS warp-zoom into live video. Was disabled at 0 (974c86e):
        // it was written for a handoff out of the idle title, and the
        // acquiring screen claimed the same 450ms window, so the two fought
        // over it. Fixed properly now - acquiring and holding both exclude
        // VideoState::TRANSITION (render_gl) instead of the warp being turned
        // off - so this can carry the real duration again.
        static constexpr float TRANSITION_DURATION_US = 450000.0f;

        BgVideoPlayer* bg_player_ = nullptr;

        // CPU path (software decode) — GL_TEXTURE_2D, BGRA
        GLuint         bg_video_tex_      = 0;
        GLuint         warp_shader_prog_  = 0;

        // DRM/EGL zero-copy path (hardware decode) — GL_TEXTURE_EXTERNAL_OES, NV12
        GLuint         bg_ext_tex_        = 0;  // GL_TEXTURE_EXTERNAL_OES
        GLuint         bg_ext_shader_     = 0;  // plain draw, samplerExternalOES
        GLuint         warp_ext_shader_   = 0;  // warp+zoom, samplerExternalOES

        // Idle title texture ("KESTREL" rendered at high resolution for BACKGROUND state)
        GLuint idle_title_tex_    = 0;   // "fpv"
        GLuint idle_title_os_tex_ = 0;   // "OS", drawn in the theme accent
        int    idle_title_w_   = 0;
        int    idle_title_h_   = 0;

        // EGL extension function pointers (loaded once in init_shaders)
        PFNEGLCREATEIMAGEKHRPROC               pfn_eglCreateImageKHR            = nullptr;
        PFNEGLDESTROYIMAGEKHRPROC              pfn_eglDestroyImageKHR           = nullptr;
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC    pfn_glEGLImageTargetTexture2DOES = nullptr;
        // Native fences (EGL_ANDROID_native_fence_sync): profiling only.
        PFNEGLCREATESYNCKHRPROC                pfn_eglCreateSyncKHR             = nullptr;
        PFNEGLDESTROYSYNCKHRPROC               pfn_eglDestroySyncKHR            = nullptr;
        PFNEGLDUPNATIVEFENCEFDANDROIDPROC      pfn_eglDupNativeFenceFDANDROID   = nullptr;

        void draw_bg_png(float fw, float fh, const float* mvp, float warp_t = 0.0f);
        void draw_bg_video(float fw, float fh, const float* mvp, float warp_t);
        // Import a player's newest decoded picture (an NV12 DMA-BUF) as the
        // external texture `tex`, without a copy. False when there is no new one.
        bool import_drm_frame(BgVideoPlayer* player, GLuint tex);

        // --- Gallery (gallery.hpp has the logic, osd_gallery.cpp draws it) ---
        // Back on the live picture opens it; while it is open it draws the
        // whole frame in place of the HUD, and the key handler hands it the keys.
        Gallery gallery_;
        // The recording being played: its own decoder, its latest picture as
        // an external texture. Opened and closed on the render thread.
        std::unique_ptr<BgVideoPlayer> clip_player_;
        std::string clip_playing_;
        GLuint      clip_ext_tex_ = 0;
        // The live picture again, as a texture (the stats screen shows it next to the charts
        // while the video plane shows it in the strip's live tile).
        bool        live_ext_have_ = false;
        uint64_t    live_ext_pts_ = 0;
        uint32_t    live_ext_epoch_ = 0;
        bool        import_live_picture();
        bool        clip_have_frame_ = false;
        // Thumbnails on the GPU, loaded as the strip scrolls to them.
        struct GalleryThumb {
            GLuint   tex = 0;
            int      w = 0, h = 0;
            uint64_t used = 0;          // frame counter, for evicting the stalest
            uint64_t next_try_us = 0;   // a thumbnail still being made is looked for again later
        };
        std::map<std::string, GalleryThumb> gallery_thumbs_;
        uint64_t gallery_frame_ = 0;
        // Where the live picture's tile is in the frame being drawn; handed to
        // the display with the frame (DrmDevice::stage_tile).
        DrmDevice::Tile frame_tile_;
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
        bool flip_tick_ = false;                // a flip landed (under osd_mutex)
        void on_flip();
        std::atomic<uint64_t> latency_frames_total_{0};   // pictures rendered, ever (KESTREL_PROF)
        std::atomic<uint64_t> last_flip_seen_us_{0};
        int since_flip_hist_[7] = {0, 0, 0, 0, 0, 0, 0};   // KESTREL_PROF
        GLuint            gallery_snap_tex_ = 0;
        bool              gallery_snap_valid_ = false;
        GLuint            gallery_snap_shader_ = 0;   // a plain premultiplied RGBA texture draw
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
    size_t   stats_frames_head_ = 0, stats_flips_head_ = 0;
    uint64_t stats_frames_t_ = 0, stats_flips_t_ = 0;       // the newest entry held
    void stats_history_update(uint64_t now_us);
    float stats_ymax_ = 0, stats_lmax_ = 0;     // the chart scales, eased
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
    GLuint stats_fbo_ = 0, stats_layer_tex_ = 0;
    int    stats_layer_w_ = 0, stats_layer_h_ = 0;
    GLuint gallery_flat_shader_ = 0, gallery_flat_vbo_ = 0;
    GLint  flat_mvp_ = -1, flat_pos_ = -1, flat_col_ = -1;
    uint64_t stats_clock_us_ = 0;               // the chart's time base (see render_gallery)
    double stats_dt_us_ = 16667.0;
    int gallery_dt_hist_[6] = {0, 0, 0, 0, 0, 0};   // KESTREL_PROF: frame intervals
        void gallery_capture_frame(int screen_w, int screen_h);
        void gallery_prewarm();
        bool render_gallery(int screen_w, int screen_h);
        void service_clip_player(const Gallery::Snapshot& s);
        GLuint gallery_thumb(const std::string& name, int& w, int& h, bool may_load, bool& loaded);
        void present_gl_frame();

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
        // AR8030 camera menu: the VIDEO tab shows the camera's own settings
        // instead of the kestrel-air encoder knobs, which do not apply here.
        // Bitmask of RF controls the baseband accepted (Ar8030Source::RfCap).
        // 0 = unknown/none, which leaves the RF tab as it was for other links.
        unsigned rf_caps = 0;
        // Raw baseband ranging reading, and the zero-point calibration we
        // subtract from it. The baseband reports a stable ~110 with the units
        // side by side, which is the "close range td_out base value" the SDK's
        // dist_calc.offset is meant to remove - but BB_CFG_DISTC is refused at
        // runtime on this board, so we subtract it here instead.
        uint64_t rec_started_us = 0;   // for the REC elapsed timer
        int  link_distance_raw = -1;
        int  dist_offset = 0;
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
        // Betaflight's own OSD canvas, drawn over whichever style is on.
        // With the canopy up, the elements the canopy already shows are
        // left out of it (MspOsd's owned-cell mask); with the HUD off it
        // is drawn whole.
        bool bf_osd = true;
        bool menu_bf_osd = true;
        // Canopy headline voltage: 0 per cell, 1 whole pack. Per cell by
        // default - it is the same number on every pack size, where a pack
        // reading means nothing until you know the cell count.
        int  volt_mode = 0;
        int  menu_volt_mode = 0;
        int  menu_wifi_ap = 0;     // SYSTEM > WiFi AP (fpvos-wifi)
        // SYSTEM > Screen Mode: 0 is Auto, then the connector's modes as
        // screen_modes() lists them. -1 until first read from screen_mode.
        int  menu_screen_mode = -1;
        std::string screen_id_, screen_key_ = "screen_mode";
        bool        screen_confirm_ = false;          // a mode on trial, asking
        std::string screen_confirm_mode_;             // it: "WxH@R", or "auto"
        uint64_t    screen_confirm_deadline_ms_ = 0;
        int         screen_confirm_left_s();
        struct ScreenModeOpt { int w, h, hz; };
        std::vector<ScreenModeOpt> screen_modes_;
        const std::vector<ScreenModeOpt>& screen_modes();
        int  screen_mode_saved();              // screen_mode's menu index
        int  screen_mode_menu();               // menu_screen_mode, read in
        std::string screen_mode_label(int idx);
        // Last sysfs read of this board's SoC temperature, cached by the top
        // status row so the canopy panel does not read thermal_zone0 again.
        float vrx_temp_cached_ = 0.0f;
        // Same reason as vrx_temp_cached_: sampled on the status row's timer
        // so the canopy panel does not re-read the ADC every frame.
        float vrx_volts_cached_ = 0.0f;
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
        unsigned frames_since_connect_   = 0;
        // Which aircraft the screen on display belongs to. Keyed on the peer
        // MAC rather than on the link coming up, because the air unit only
        // resends a DisplayPort row whose content CHANGED - see
        // update_artosyn_stats().
        char     osd_peer_mac_[20]       = {0};
        // The acquiring screen runs for at least this long from the link edge,
        // the same edge the canopy's rebuild sweep fires on. Without it the
        // screen is hostage to how fast the air unit dumps its startup buffer:
        // frames captured 16.7ms apart arrive 0-4ms apart, so ten of them land
        // inside a few milliseconds and the screen is gone before it is seen.
        static constexpr uint64_t kLockMinUs  = 1200000;   // 1.2s on screen
        static constexpr uint64_t kLockFadeUs = 400000;    // last 0.4s fades
        uint64_t lock_hold_until_us_ = 0;
        GLuint clock_tex = 0;   // canopy icons are vectors now (vec_icons.hpp)
        float vtx_cpu_c = 0.0f;   // air-side SoC, cmd 0x05 byte 1
        float rsm_latency_ms = 0.0f;  // AU reassembly, from Ar8030Source
        float air_delay_ms = 0.0f;    // air capture -> ground arrival
        bool  air_delay_valid = false;
        bool  vtx_temp_valid = false;
        int menu_ar_power = 500;   // mW, stock's encoding (see kArPwrLevels)
        int menu_ar_chan  = -1;   // -1 = AUTO
        bool menu_ar_hop  = true;  // channel hopping (AUTO/ACS)
        int menu_ar_maxbr = 0;     // index into kArMaxBrVals (0 = AUTO, no cap)
        int menu_cam_ev       = 3;  // index into kEvSteps below (3 = 0.0 EV)
        int menu_cam_sat      = 0;
        int menu_cam_contrast = 0;  // 0-15: the config packs it into a nibble
        int menu_cam_sharp    = 0;
        int menu_cam_scene    = 0;
        int menu_cam_wb       = 0;
        int menu_cam_3dnr     = 0;   // index into kDnrVals
        int rf_bw_applied = -1;  // bandwidth re-asserted this link session (-1 = pending)
        bool rf_chan_asserted = false; // channel re-asserted this link session
        int  air_chan_mode = -1;       // AIR-owned chan mode from broadcast (1=auto 0=force)
        bool menu_scan_open = false;   // RF tab: channel-scan (spectrum) sub-screen open
        int  menu_scan_sel = 0;        // channel-scan: highlighted channel (sorted by freq)
        bool air_standby = false;      // as REPORTED by the air unit (TLV 0x12)
        bool menu_ar_standby = false;  // the menu's value: seeded from air_standby,
                                       // toggled to force standby on/off (momentary,
                                       // never persisted - the air still overrides it)
        // Whether the VTX is ACTUALLY drawing standby power, inferred from
        // bytes 29-30 of the cmd 0x05 VTX telemetry frame rather than from
        // the TLV 0x12 standby report above - which was confirmed stuck
        // reporting ON for an entire session while this signal correctly
        // tracked a real arm/disarm transition (temperature climbing
        // 47C->64C in lockstep). See ar8030_source.cpp and
        // vtx-standby-detection-via-cmd05-bytes2930.md.
        bool vtx_low_power = false;
        int  menu_brightness = 50;     // HDMI connector brightness, 0..100
        int      scan_peak_dbm[32] = {0}; // 30s decaying max-hold energy per channel index
        uint64_t scan_peak_ts[32]  = {0}; // µs timestamp the peak was last (re)set (0=unset)
        int  pinned_ci = -1;           // radio channel index the AP is pinned to (-1 = AUTO/ACS)
        int  menu_pin_pending = -2;    // scan Enter confirm: -2=none, -1=pending release, >=0=pending pin
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

        int menu_tab = 0; // 0: Video, 1: RF

        bool menu_synced = false;
        bool show_latency_graph = false; // Default OFF
        bool show_all_adapters = false; // Default OFF
        bool bg_video_enabled = true;   // Default ON — use background.mp4
        // Reactive HUD intensity: 0=OFF 1=SMALL 2=MEDIUM 3=EXTREME.
        // MEDIUM is the tuned default (see kBankGain/kMaxOffset in msp_osd).
        int  hud_reactivity = 2;

        bool menu_show_latency_graph = false;
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
        static const int kMenuTabs = 6, kMenuRows = 24;
        static const int kTabDvr = 4, kTabSystem = 5;
        // DISPLAY rows, in the order they are listed. The tab is dispatched
        // by index in half a dozen places, so the order lives here once and
        // the dispatchers name the row rather than count to it.
        enum DisplayRow {
            kDispTheme = 0, kDispBgVideo, kDispPicture, kDispUiScale, kDispBrightness
        };
        // HUD rows, headers included, in the order they are listed.
        enum HudRow {
            kHudHdrOverlay = 0, kHudRowStyle, kHudRowBfOsd,
            kHudHdrReadouts, kHudRowVoltage, kHudRowGraph, kHudRowCalib,
            kHudHdrScreen, kHudRowClock,
            kHudHdrAttitude, kHudRowDynamic
        };
        // A row's help line, which for a few rows depends on the value it is
        // showing; everything else gets its MenuItem::help back.
        const char* menu_help_text(int tab, int i, const char* fallback);
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
        bool menu_show_all_adapters = false;
        bool menu_bg_video = true;
        int  menu_hud_reactivity = 2;
        // DVR Source row: 0 FPV, 1 SCREEN at 30 captures/s, 2 SCREEN at 60.
        int  menu_dvr_source = 0;
        int  dvr_source_now() const;
        std::function<void(int, int)> cmd_cb;
        int drift_request = 0;

        std::string decoder_name;
        std::shared_ptr<DVR> dvr;
        bool dvr_screen = false;
        pthread_t tid_dvr_menu;
        bool tid_dvr_menu_active = false;
        GLuint live_ext_tex_ = 0;
        GLuint live_video_tex_ = 0;

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
        void draw_menu(math::Mat4& mvp, float fw, float fh);
        // The three-column body of it (hud_menu.cpp).
        void draw_menu_columns(math::Mat4& mvp, float fw, float fh);
        // Channel-scan sub-screen: spectrum of per-channel busy-ness. Drawn inside
        // the menu panel frame (mx,my,mw,mh) when menu_scan_open is set.
        void draw_chan_scan(float mx, float my, float mw, float mh, float s_menu);
    public:
        int refresh_frequency_ms;
        void update_menu_from_tx(int cmd, int val);
        uint32_t get_sky_exposure_us() {
            pthread_mutex_lock(&osd_mutex);
            uint32_t val = osd_vars.sky_exposure_us;
            pthread_mutex_unlock(&osd_mutex);
            return val;
        }
        uint32_t get_sky_framerate() {
            pthread_mutex_lock(&osd_mutex);
            uint32_t val = osd_vars.sky_framerate;
            pthread_mutex_unlock(&osd_mutex);
            return val;
        }

    private:
        void init_gl_buffers();
        void init_shaders();
        void render_gl();
        GLuint load_texture_from_png(const unsigned char* png, unsigned int length);
        GLuint load_texture_from_file(const char* filename);
        void draw_debug_chart();
        void draw_panel(float x, float y, float w, float h, float alpha, float r = 0.1f, float g = 0.1f, float b = 0.15f);
        void draw_trapezoid(float x, float y, float w, float h, float top_scale, float slant, float alpha, float r, float g, float b);
        void draw_icon(GLuint tex, float x, float y, float w, float h, float r = 1.0f, float g = 1.0f, float b = 1.0f);
        void ensure_text_texture(const char* text);
        // Rendered width of `text` at `scale`, in the same units draw_text uses
        // for right-alignment. Lets a right-aligned line be built from several
        // differently-coloured segments.
        float text_width(const char* text, float scale);
        void draw_text(const char* text, float x, float y, float scale, bool right_align = false, float r = 1.0f, float g = 1.0f, float b = 1.0f);
        // Wipe-reveal variant: only the left `reveal` fraction (0..1) of the
        // string is drawn, as a texture crop rather than a fade - see the
        // .cpp for why this can reuse draw_text's own cached texture.
        void draw_text_wipe(const char* text, float x, float y, float scale, bool right_align, float r, float g, float b, float reveal);
        void draw_icon_quad(GLuint tex, math::Vec2 bl, math::Vec2 br, math::Vec2 tr, math::Vec2 tl, float r = 1.0f, float g = 1.0f, float b = 1.0f);
        
        // Visual Telemetry Helpers
        void draw_graph(float x, float y, float w, float h, const std::vector<float>& data, float max_val, float r, float g, float b);
        void draw_latency_graph(float x, float y, float w, float h, const std::vector<LatencyFrame>& data, float max_val, int fps = 120);
        void draw_bitrate_graph(float x, float y, float w, float h, const std::vector<LatencyFrame>& data, float max_val);
        void draw_latency_health_bar(float x, float y, float h, const LatencyFrame& frame);
        void draw_stacked_bar(float x, float y, float w, float h, float slant, bool reverse, float val1, float r1, float g1, float b1, float val2 = 0, float r2 = 0, float g2 = 0, float b2 = 0, float val3 = 0, float r3 = 0, float g3 = 0, float b3 = 0, float val4 = 0, float r4 = 0, float g4 = 0, float b4 = 0, float val5 = 0, float r5 = 0, float g5 = 0, float b5 = 0, float val6 = 0, float r6 = 0, float g6 = 0, float b6 = 0);
        void draw_signal_bar(float x, float y, float w, float h, float value, float min_val, float max_val);
        
        void draw_poly(const std::vector<math::Vec2>& points, float alpha, float r, float g, float b);
        // uniform_x: drop the left-to-right fade and use the mid-width value
        // across the whole panel. The wing panels want the gradient (it points
        // at the screen edge); the centre menu does not - it made one side of
        // the menu wash out over video.
        void draw_hex_panel(float x, float y, float w, float h, float alpha, float r, float g, float b, bool outline, int num_h = 8, int num_v = 10, float grid_y = -1.0f, float grid_h = -1.0f, bool uniform_x = false);
        void draw_arc(float cx, float cy, float r_in, float r_out, float start_angle, float end_angle, float alpha, float r, float g, float b);
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

        void draw_grid_arc(float cx, float cy, float r_inner, float r_outer, float start_angle, float end_angle, float alpha);
        void draw_glow_line(const std::vector<math::Vec2>& points, float width, float alpha, float r, float g, float b);

        // --- CANOPY HUD (hud_canopy.cpp) ---
        // Everything the canopy layout draws, gathered at the one point in
        // render_gl() that already holds the snapshots. Passing a struct keeps
        // the signature from growing a parameter every time a slot is added,
        // and keeps the layout code out of the 5000-line render_gl().
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
            float mcs_norm  = 0.0f;      // MCS rung / rungs
            float quality   = 0.0f;      // composite link health, 0..1
            int   link_warn = 0;         // 0 fine, 1 amber, 2 red (see link_warn_hold)
            int   dist_m    = -1;        // -1 = no ranging
            char  vtx_pwr[24] = {0};
            int   link_freq_mhz = 0;     // 0 = unknown, do not show
            // What the video link is actually delivering: the mode it is
            // sending, the bitrate it is spending on it, and how long a frame
            // takes to arrive. The three belong on one line because no one of
            // them means much alone - 18 Mbps is generous at 1080p60 and thin
            // at 1080p120.
            int   v_w = 0, v_h = 0, fps = 0;
            float mbps = 0.0f;
            bool  have_lat = false;
            float lat_med_ms = 0.0f;
            bool  vtx_temp_valid = false;
            float vtx_temp_c = 0.0f;
            // Live standby, not the TLV enable/disable setting - see
            // OSD::vtx_low_power and ar8030-power-and-standby-verified.md.
            bool  vtx_low_power = false;
            bool  rec_active = false;
            unsigned rec_secs = 0;
            // Left panel
            BfTelem bf;
            float vrx_temp_c = 0.0f;
            // This goggle's own supply voltage, read from the baseband's ADC
            // (see Ar8030Source::vrx_volts). 0 = not read yet / unavailable,
            // and the HUD draws nothing rather than inventing a reading.
            float vrx_volts  = 0.0f;
        };
        void draw_canopy_hud(const CanopyIn& in);
        // A run of tapering blocks, the canopy's only gauge form. `edge_left`
        // puts the tall end at the left (screen) edge and fills rightwards;
        // false mirrors it. `filled` is 0..1 of the run.
        // `reveal` limits how much of the run is emitted at all, measured from
        // the outboard end - the link-acquired sweep, as distinct from
        // `filled`, which is the reading.
        void draw_seg_track(float x, float y, float w, float h, int segs,
                            float filled, bool edge_left,
                            float r, float g, float b, float a_lo, float a_hi,
                            float reveal = 1.0f);
        // The panel ground: a wedge that fades out towards the screen centre.
        // Columns of a fading shape, already in frustum space: xs[i] with its
        // top and bottom edge and the alpha at that column.
        float aa_feather_ = 0.0f;   // edge feather width, canopy units (hud_canopy.cpp)
        void draw_fade_wedge(const float* xs, const float* tops, const float* bots,
                             const float* alphas, int n, float r, float g, float b);
        // A hairline that ramps from a0 at one end to a1 at the other, used
        // for the panel rules. Two alphas rather than one so a rule can be cut
        // in half mid-fade without the join showing.
        void draw_fade_rule(float x0, float y0, float x1, float y1, float thick,
                            float a0, float r, float g, float b, float a1 = 0.0f);
        // Glowing rails along the picture's top and bottom edges: `alpha` at
        // the frame edge, fading to nothing `depth` inboard. The video link
        // warning.
        void draw_rail_glow(float fw, float fh, float depth, float alpha,
                            float r, float g, float b);
        // Video link warning level for this frame: 0 fine, 1 amber, 2 red.
        // Worse levels land at once; a better one has to hold before the
        // warning steps down, so a link hovering on a threshold does not
        // flash the screen edge.
        int  link_warn_hold(int raw);
        int  link_warn_level_  = 0;
        uint64_t link_warn_better_us_ = 0;
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
        bool     msp_wake_     = false;   // signalled (throttled to 30 Hz)
        bool     msp_dirty_    = false;   // data waiting, signalled or not
        uint32_t last_msp_ver_ = 0;
        std::atomic<uint64_t> last_msp_signal_us_{0};
        uint64_t next_render_interval_us();

        void run();

    public:
        OSD(std::shared_ptr<DrmDevice> dev_, int refresh_frequency_ms_, volatile bool* signal_stop, bool console_stats_ = false);
        ~OSD();
        void signal_render(prof::Wake why = prof::kWakeOther);

        void update_stats(int current_framerate, latency_stats stats);
        void add_latency_frame(LatencyFrame frame);
        void set_video_resolution(int w, int h);
         void update_video_bandwidth(float bw);
          void set_slices_received(bool val);
          bool get_slices_received();
          void update_link_stats(packets_stats v);
        // The Back button (and Esc on a keyboard). On the live picture it opens
        // the gallery; with the menu open it closes it, as M does.
        static constexpr int kKeyBack = 0x105;
        void handle_key(int key);
        void set_ui_scale(float v);
        void set_decoder_name(std::string name) { decoder_name = name; }
        // The screen in use (utils/screen_id.h, "" if none) and the settings
        // key its mode is kept under: screen_mode_<ID>, or screen_mode.
        void set_screen(const std::string& id, const std::string& key) { screen_id_ = id; screen_key_ = key; }
        // This run's mode is on trial (screen_mode_try, main.cpp): the menu
        // opens on SYSTEM > Screen Mode to ask whether to keep it. Enter keeps
        // it; otherwise, or unseen on a black screen, it is dropped and the
        // app restarts in the screen's kept mode.
        void begin_screen_mode_confirm(const std::string& mode);
        std::string get_decoder_name() const { return decoder_name; }
        void set_command_callback(std::function<void(int, int)> cb) { cmd_cb = cb; }
        void set_rf_caps(unsigned caps) { rf_caps = caps; }
        // Air unit reports low-power/standby (sky cmd 0x03, TLV tag 0x12).
        // Keep the menu value in step with the air's own report so the row shows
        // the current status - but not while the menu is open, so a force the
        // user is dialling in is not clobbered by an incoming status frame.
        void set_air_standby(bool on) { air_standby = on; if (!menu_open) menu_ar_standby = on; }
        void set_vtx_low_power(bool on) { vtx_low_power = on; }
        // Air<->ground distance from the baseband's time-of-flight ranging,
        // metres; -1 when it has no fix. Published by Ar8030Source.
        void set_link_distance(int raw) { link_distance_raw = raw; }
        // VTX (air unit) SoC temperature in °C, from sky cmd 0x05 byte 1.
        // No VTX RF board reading yet:
        // byte 33 was tempting but turned out to be TX gain (stock's OSD:
        // "txgain:40,39") - it dropped with TX power, so I read it as PA
        // cooling; wrong.
        void set_vtx_temp(float cpu) { vtx_cpu_c = cpu; vtx_temp_valid = true; }
        // Access-unit reassembly time in ms (first slice of a picture on the
        // wire to last), published by Ar8030Source. Real measurement, unlike
        // the cap/enc/net components that died with the RTP pipeline.
        void set_reassembly_latency(float ms) { rsm_latency_ms = ms; }
        // Air-unit capture to ground arrival, ms. Derived from the per-frame
        // video header's capture timestamp against the BB_GET_AP_TIME clock -
        // the half stock shows that the baseband SDK does not expose.
        void set_air_delay(float ms) { air_delay_ms = ms; air_delay_valid = true; }
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
        struct MenuItem { const char* label; int type; const char* help = nullptr;
                          const char* hint = nullptr; };
        void menu_action_status(int tab, int i, char* hint, size_t hcap,
                                char* status, size_t scap);
        std::vector<MenuItem> menu_items(int tab) const;
        void set_video_mode_names(const std::vector<std::string>& n, int current) {
            video_mode_names = n;
            if (current >= 0 && current < (int)n.size()) menu_video_mode = current;
        }
        // The list is published before the link is up, when the per-air-unit
        // config has not been read yet, so the row would otherwise sit on the
        // built-in default forever. Called again once load_sky_config() knows
        // which mode is actually being pushed.
        // Seed the camera rows from the config that was actually restored and
        // pushed. Without it every row shows a compile-time default no matter
        // what the air unit is running - the same fault the Video Mode row had.
        // Raw wire values in; the index lookups into the label tables happen in
        // the .cpp, where those tables live.
        void set_camera_config(int ev_x10, int sat, int contrast, int sharp,
                               int scene, int awb, int angle,
                               int dnr3d, int focus);
        void set_video_mode_current(int current) {
            pthread_mutex_lock(&osd_mutex);
            if (current >= 0 && current < (int)video_mode_names.size())
                menu_video_mode = current;
            pthread_mutex_unlock(&osd_mutex);
        }
        int get_drift_request() { int d = drift_request; drift_request = 0; return d; }
        // Called from the AR8030 RX thread. It only copies - parsing happens
        // on the OSD thread in drain_msp(). The RX thread also feeds the
        // decoder, and doing OSD work on the latency-critical video path was
        // both wrong by design and correlated with a SIGSEGV in the renderer's
        // frame queue.
        void update_msp_data(const uint8_t* data, size_t len);
    private:
        std::mutex msp_rx_mtx;
        std::vector<uint8_t> msp_rx_buf;
        unsigned long long msp_rx_dropped = 0;
        void drain_msp();
    public:
        void update_artosyn_stats(artosyn_stats v);
        // Link state alone, for callers that run on a timer rather than on
        // frame arrival - see the definition.
        void update_artosyn_link_state(int state);
        // The air handshake has gone out. Only the acquiring screen reads it,
        // to tell "link up, nothing asked for yet" from "asked, nothing back".
        void notify_handshake_sent();
        void set_chan_scan(const chan_scan_info& sc);  // push spectrum scan (RPC thread)
        bool video_decoding();   // decoded FPV frame within the last 1.5s
        void update_adapt_stats(adapt_stats v);   // air RF adaptation telemetry
        void log_csv_row();                        // append a link/adaptation CSV row
    private:
        void* csv_fp = nullptr;                    // FILE* for /tmp/kestrel-adapt.csv (lazy-opened)
    public:
        void set_dvr(std::shared_ptr<DVR> dvr_, bool dvr_screen_);
        bool is_dvr_screen_enabled() { return dvr_screen; }

        void stop();
        // Called by renderer thread every time a FPV frame is successfully flipped.
        void notify_video_frame(bool is_keyframe = false);

        static void* run_thread(void* arg) {
            static_cast<OSD*>(arg)->run();
            return nullptr;
        }

    
};

#endif

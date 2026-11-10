// One frame of the HUD: render_frame(), and what it takes and works out once
// (the Frame) for the screens to draw from: the GL and the matrices, the
// snapshot under osd_mutex, the video's state, the panels' matrices, the
// link as measured.
#include "hud/osd_detail.hpp"
#include "system/gnd/link_range.hpp"

void OSD::render_frame() {
    if (!layer_ready_) return;
    frame_timers();                // whatever is on the screen, the gallery too
    Frame f;
    if (!frame_begin(f)) return;   // the gallery took the frame
    frame_take(f);
    frame_video(f);
    draw_backdrop(f);
    draw_idle_title(f);
    draw_shade(f);
    frame_wings(f);
    draw_top_strip(f);
    draw_bind(f);
    measure_link(f);
    draw_arena_left(f);
    draw_arena_right(f);
    draw_canopy(f);
    draw_betaflight(f);
    draw_link_edge(f);
    draw_menu(f);
    if (gallery_capture_) gallery_capture_frame(f.screen_w, f.screen_h);   // before the swap
    present_frame();
}

bool OSD::frame_begin(Frame& f) {
    auto& screen_w = f.screen_w;
    auto& screen_h = f.screen_h;
    auto& projection = f.projection;
    auto& view = f.view;
    auto& tmp = f.tmp;
    auto& model_c = f.model_c;
    auto& model_hud_c = f.model_hud_c;
    auto& mvp_c = f.mvp_c;
    auto& mvp_hud_c = f.mvp_hud_c;
    auto& hud_dx = f.hud_dx;
    auto& hud_dy = f.hud_dy;
    auto& hud_bank = f.hud_bank;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& hud_margin_l = f.hud_margin_l;
    auto& hud_margin_r = f.hud_margin_r;
    auto& hud_margin_b = f.hud_margin_b;
    prof::frame_begin();
    layer_->begin();   // the drawing to the layer's next buffer

    const display::Mode screen = dev->mode();
    screen_w = screen.w;
    screen_h = screen.h;
    canvas_->begin_frame(screen_w, screen_h);   // all of it cleared, every frame

    // The gallery (Back on the live picture) takes the whole frame: no HUD,
    // no picture scale. It hands back false once it has closed.
    frame_tile_ = display::Tile();
    if (!gallery_.active()) phase_lock_.store(false, std::memory_order_relaxed);
    if (prof::enabled() && !gallery_.active()) stats_prof(get_time_us());
    if (gallery_.active() && render_gallery(screen_w, screen_h)) {
        present_frame();
        return false;
    }

    // Clip the whole overlay to the scaled picture. The canopy's wings are
    // drawn to run PAST the physical screen edge on purpose, so the edge
    // clips their outer ends - and shrinking everything in clip space brought
    // that overhang inside the picture. Rather than teach each element about
    // the new edge, scissor to the picture rectangle: nothing outside it is
    // drawn, whatever it is. After the clear, so the area outside is still
    // cleared each frame and cannot hold stale pixels; at 100% this is the
    // full screen and changes nothing. Same rectangle the video plane uses.
    {
        int px, py, pw, ph;
        if (dev) dev->picture_rect(screen_w, screen_h, px, py, pw, ph);
        else { px = 0; py = 0; pw = screen_w; ph = screen_h; }
        canvas_->clip(px, py, pw, ph);   // from the bottom; the rect is centred, so symmetric
    }
    canvas_->blend_over();

    math::perspective(projection, 0.785f, (float)screen_w / screen_h, 0.1f, 100.0f);

    // Whole-picture scale. The video plane is shrunk by the display
    // controller (drm.cpp); the OSD plane cannot be - it sits on a Smart
    // window with no scaler - so the overlay shrinks itself to match, here,
    // in the one projection every MVP on this frame is built from. Scaling
    // the projection's x and y rows is exactly "gl_Position.xy *= s".
    //
    // It is done on the CPU on purpose. The first version did the multiply
    // in the vertex shaders, and on this Mali driver that line alone - even
    // at a scale of 1.0 - left white text and the idle title drawn faint,
    // as if their alpha had been cut. Keeping the shaders untouched avoids
    // whatever the compiler made of it; the matrix costs nothing.
    {
        const float s = dev ? dev->picture_scale() / 100.0f : 1.0f;
        projection[0] *= s;   // x row: the only non-zero entry is [0]
        projection[5] *= s;   // y row: the only non-zero entry is [5]
    }
    math::lookAt(view, {0, 0, wrap_depth}, {0, 0, 0}, {0, 1, 0});
    
    math::multiply(tmp, view, projection);
    
    // Fetch dynamic HUD displacement bounce offsets from active MSP telemetry.
    // hud_bank is a view-plane tilt from the craft's roll - a bank angles the
    // HUD rather than sliding it, which would read as yaw.
    // Only while there is an aircraft. In IDLE the last attitude describes a
    // craft that is no longer there, and holding the panels at its offset (or
    // springing them about on whatever stale reading survived) reads as a
    // stuck HUD - so centre them and leave them still. hud_connected_ is a
    // frame behind here, which is not visible.
    msp_osd.set_motion_wanted(hud_reactivity > 0 && hud_connected_);
    if (hud_reactivity > 0 && hud_connected_) {
        // 1=SMALL 2=MEDIUM 3=EXTREME. MEDIUM is 1.0 - the amount tuned
        // against a real craft, so the other two hang off it.
        static const float kReactScale[4] = { 0.0f, 0.5f, 1.0f, 3.0f };
        bool moving = false;
        msp_osd.get_hud_offset(hud_dx, hud_dy, hud_bank, kReactScale[hud_reactivity], moving);
        // Still settling: keep frames coming until it is at rest.
        if (moving) signal_render(prof::kWakeAnim);
    } else {
        msp_osd.reset_hud_motion();   // leaves dx/dy/bank at 0
    }

    // 1. Center Matrix (No rotation) - Background remains stable (mvp_c)
    math::rotateY(model_c, 0.0f);
    math::multiply(mvp_c, model_c, tmp);

    // 1b. Center HUD Matrix (dynamic bank + translation shift)
    math::rotateZ(model_hud_c, hud_bank);
    model_hud_c[12] = hud_dx;
    model_hud_c[13] = hud_dy;
    math::multiply(mvp_hud_c, model_hud_c, tmp);

    // --- DYNAMIC BOUNDARY CALCULATION ---
    float fov_y = 0.785f; // ~45 deg
    float aspect = (float)screen_w / (float)screen_h;
    
    // Absolute 100% screen boundaries in 3D units at wrap_depth
    frustum_h = tanf(fov_y / 2.0f) * wrap_depth;
    frustum_w = frustum_h * aspect;

    // HUD Margins (Distance from the physical screen edges)
    // 0.0f = Absolute edge. Increase to move elements inward.
    // A fixed inset from the edge. (There used to be a user "overscan" here
    // that pushed the HUD inward; Picture Size replaced it, since shrinking
    // the whole picture is what actually helps optics that clip the frame.)
    hud_margin_l = 0.02f;
    hud_margin_r = 0.02f;
    hud_margin_b = 0.02f;
    return true;
}

// What runs on the clock, not on what is drawn: the trials' reverts and the
// time zone. The gallery takes the whole frame (frame_begin) and frame_take
// does not run under it, so with these there a trial outlived its 10 or 15 s
// for as long as the gallery stayed open.
void OSD::frame_timers() {
    const int arm = msp_osd.get_telem().arm;   // its own lock, before ours
    pthread_mutex_lock(&osd_mutex);
    // Armed: the menu gets out of the way, wherever it is - a blade picked,
    // a row, a value being changed, the channel page. Only on the arming
    // itself, so it can still be opened while armed (on the bench). A screen
    // mode on trial is kept rather than restarting the app in the air. The
    // arming is a change from disarmed: not the first word after a start (a
    // trial's restart with the craft armed on the bench would keep a mode
    // nobody has seen), nor one after the FC went quiet (-1) while armed.
    if (arm == 1 && arm_seen_ == 0 && menu_open) {
        if (screen_confirm_) {
            screen_confirm_ = false;
            settings_.set(display::setting::screen_mode(screen_id_),
                screen_confirm_mode_ == "auto" ? std::string() : screen_confirm_mode_);
            printf("menu: screen mode %s kept for %s (armed)\n", screen_confirm_mode_.c_str(),
                   screen_id_.empty() ? "this screen" : screen_id_.c_str());
        }
        menu_open = false;
        menu_scan_open = false;
        ui_stage_ = 0;
        menu_focus = 0;
        printf("menu: closed - armed\n");
        render_requested = true;
    }
    if (arm >= 0) arm_seen_ = arm;
    // SYSTEM > Time Zone, picked in the menu or from a phone: its TZ goes into
    // force here, on the thread that draws the clock (setenv() must not race
    // the time functions), and the menu's mirror follows a change made elsewhere.
    zones::apply_pending_tz();
    if (zones::generation() != menu_zone_gen_) {
        menu_zone_gen_ = zones::generation();
        if (menu_zone_ != zones::selected()) {
            menu_zone_ = zones::selected();
            menu_opt_tab_ = -1;
            // A zone picked elsewhere (the web page) can take the Timezone row
            // away from under the selection.
            if (menu_open && menu_tab == kTabSystem) {
                const int n = (int)menu_items(kTabSystem).size();
                if (menu_index >= n) {
                    menu_index = n - 1;
                    if (menu_focus == 2) menu_focus = 1;
                }
            }
            if (menu_open) menu_refresh_options();
        }
    }
    // Refresh Sync on trial that nobody kept: back to the kept value.
    if (sync_confirm_ && sync_confirm_left_s() <= 0) {
        sync_confirm_ = false;
        printf("menu: refresh sync %s not kept - back to %s\n", refresh_sync_label(refresh_sync().mode),
               refresh_sync_label(sync_confirm_prev_));
        menu_refresh_sync = sync_confirm_prev_;
        if (dev) {
            dev->set_sync_mode(sync_confirm_prev_);
            dev->refollow();
        }
        // Back to OFF takes the Sync Margin row away, maybe from under the selection.
        if (menu_open && menu_tab == 3) {
            const int n = (int)menu_items(3).size();
            if (menu_index >= n) {
                menu_index = n - 1;
                if (menu_focus == 2) menu_focus = 1;
            }
        }
        if (menu_open) menu_refresh_options();   // the list's position too, not just the row's label
        render_requested = true;
    }
    // A screen mode on trial that nobody kept: back to the screen's kept mode.
    if (screen_confirm_ && screen_confirm_left_s() <= 0) {
        screen_confirm_ = false;
        printf("menu: screen mode %s not kept - restarting in the previous one\n",
               screen_confirm_mode_.c_str());
        kestrel_request_restart();
    }
    pthread_mutex_unlock(&osd_mutex);
}

void OSD::frame_take(Frame& f) {
    auto& tmp = f.tmp;
    auto& model_hud_c = f.model_hud_c;
    auto& model_top = f.model_top;
    auto& mvp_top = f.mvp_top;
    auto& last_latency_frame = f.last_latency_frame;
    auto& have_latency_frame = f.have_latency_frame;
    auto& latency_median_ms = f.latency_median_ms;
    auto& have_latency_median = f.have_latency_median;
    auto& latency_slice_ms = f.latency_slice_ms;
    auto& latency_nslices = f.latency_nslices;
    auto& link_stats_copy = f.link_stats_copy;
    auto& v_width = f.v_width;
    auto& v_height = f.v_height;
    auto& cur_fps = f.cur_fps;
    auto& v_bw = f.v_bw;
    auto& lat_avg = f.lat_avg;
    auto& lat_max = f.lat_max;
    auto& slices_received = f.slices_received;
    auto& menu_moving = f.menu_moving;
    auto& is_menu_open = f.is_menu_open;
    auto& s = f.s;
    auto& hud_panels = f.hud_panels;
    auto& canopy_on = f.canopy_on;
    auto& legacy_panels = f.legacy_panels;
    auto& hud_verbose = f.hud_verbose;
    auto& legacy_verbose = f.legacy_verbose;
    auto& link_copy = f.link_copy;
    // --- THREAD SAFETY: Copy shared state ---
    pthread_mutex_lock(&osd_mutex);
    sync_radio();               // what the radio published since, and the renderer's latency samples
    *link_ = link_view();   // the link as this frame shows it
    // Keep the history graphs SCROLLING through dead air: when no frame has
    // arrived for 150ms, backfill zero "gap" entries at ~30/s so an outage
    // draws as a flat gap in the timeline instead of freezing the graph on
    // its last shape (the render loop still ticks via the 1s refresh/bg feed).
    {
        uint64_t now_us = get_time_us();
        uint64_t last = osd_vars.latency_history_last_us;
        if (last != 0 && now_us - last > 150000) {
            int fill = (int)((now_us - last) / 33333);   // ~30 entries per second
            if (fill > 90) fill = 90;                    // cap backlog per pass
            for (int k = 0; k < fill; k++) {
                LatencyFrame gap{};
                gap.t_us = now_us - (uint64_t)(fill - k) * 33333ULL;
                osd_vars.latency_ring.push(gap);
            }
            osd_vars.latency_history_last_us = now_us;
        }
    }
    // Only what this frame shows: the newest entry and the median.
    have_latency_frame = !osd_vars.latency_ring.empty();
    if (have_latency_frame) last_latency_frame = osd_vars.latency_ring.back();
    have_latency_median = osd_vars.latency_ring.median(latency_median_ms);
    // Each slice's too, while the ring is ours (osd_mutex): the console stats
    // can fill it from the renderer's thread.
    latency_nslices = osd_vars.latency_ring.slice_medians(latency_slice_ms);
    link_stats_copy = osd_vars.link_stats;
    v_width = osd_vars.video_width;
    v_height = osd_vars.video_height;
    cur_fps = osd_vars.current_framerate;
    v_bw = osd_vars.video_bandwidth;
    lat_avg = osd_vars.total_latency_avg;
    lat_max = osd_vars.total_latency_max;
    slices_received = osd_vars.slices_received;
    if (slices_received) {
        // the slowest slice's: enc = capture -> encoder out (air side), rf = encoder out to here, dec, disp
        lat_avg = osd_vars.tx_latency_avg
                + osd_vars.proc_latency_avg
                + osd_vars.decoding_latency_avg + osd_vars.display_latency_avg;
        if (lat_max < lat_avg) lat_max = lat_avg;
    }
    // AIR UNIT > SERIAL counts up while it is open: redraw four times a second.
    if (menu_open && menu_tab == kTabAirSerial) {
        const uint64_t t = get_time_us();
        if (t - serial_tab_us_ >= 250000ULL) { serial_tab_us_ = t; render_requested = true; }
        // The frame rate: the count a second apart.
        if (t - serial_rate_us_ >= 500000ULL) {      // twice a second, and only with this tab open
            const MspOsd::MspCounts g = msp_osd.msp_counts();
            const uint32_t n = g.frames;
            const float dt = serial_rate_us_ ? (t - serial_rate_us_) / 1e6f : 0.0f;
            serial_rate_ = (dt > 0.0f && n >= serial_prev_) ? (n - serial_prev_) / dt : 0.0f;
            serial_prev_ = n;
            serial_bps_ = (dt > 0.0f && g.bytes >= serial_prev_bytes_) ? (g.bytes - serial_prev_bytes_) / dt : 0.0f;
            serial_prev_bytes_ = g.bytes;
            serial_rate_us_ = t;
        }
    }
    {
        int tilt_deg;
        if (msp_osd.take_cam_tilt_to_save(&tilt_deg)) {
            settings_.set(hud::setting::imu_cam_tilt_deg, tilt_deg);
            printf("hud: camera tilt %d deg, taken at arming, saved\n", tilt_deg);
        }
    }
    if (menu_open_at_start) {
        menu_open = true;
        menu_open_at_start = false;
        if (screen_confirm_) {           // straight onto the question
            menu_first_open_ = false;
            menu_tab = 3;
            menu_index = kDispScreenMode;
            menu_focus = 1;
        }
        if (menu_first_open_) {          // --menu with no tab named
            menu_first_open_ = false;
            menu_tab = kTabSystem;
            menu_index = 0;
            menu_focus = 0;
        }
        // Straight onto the goggle's blade, unfolded: on DISPLAY for the
        // screen mode's question, else on SYSTEM.
        ui_side_ = 0;
        if (!screen_confirm_) menu_tab = kTabSystem;
        ui_side_tab_[0] = menu_tab;
        ui_stage_ = 1;
        if (menu_focus == 0) menu_focus = 1;
        // --menu opens without a keypress, so nothing has worked out the value
        // column yet. Safe to do from here: this runs under the same lock the
        // key handler takes, so no press can be halfway through a row.
        menu_refresh_options();
    }
    if (menu_open && screen_modes_stale_) refill_screen_modes();
    // The canopy's motion (ui_* in osd.hpp), eased the way the gallery's zoom
    // is - each value closes the gap to its target by 1 - e^(-dt/tau) a frame -
    // and kept moving a frame at a time, started right after a flip as the
    // gallery's are.
    {
        const uint64_t t_us = get_time_us();
        float dt = menu_anim_us_ ? (float)(t_us - menu_anim_us_) / 1e6f : 1.0f / 60.0f;
        menu_anim_us_ = t_us;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;   // a stall is not a leap
        auto follow = [&](float& v, float target, float tau) {
            v += (target - v) * (1.0f - std::exp(-dt / tau));
            if (std::fabs(target - v) < 0.002f) v = target;
            if (v != target) menu_moving = true;
        };
        const bool band = menu_open && menu_scan_open;
        follow(ui_wake_, menu_open ? 1.0f : 0.0f, 0.09f);
        for (int i = 0; i < 2; i++) {
            const bool picking = menu_open && ui_stage_ == 0;
            follow(ui_lift_[i], picking ? (ui_side_ == i ? 1.0f : -0.6f) : 0.0f, 0.08f);
            follow(ui_open_[i], (menu_open && ui_stage_ == 1 && ui_side_ == i && !(band && i == 1)) ? 1.0f : 0.0f, 0.09f);
        }
        follow(ui_band_, band ? 1.0f : 0.0f, 0.09f);
    }
    // Drawn while it wakes or settles, not only while it has the keys.
    is_menu_open = menu_open || ui_wake_ > 0.0f;
    s = osd_vars.ui_scale;

    // Which overlay. OFF leaves the screen to the Betaflight OSD; CANOPY
    // replaces both wings wholesale, so every arena panel element is gated
    // on `legacy_panels` while the shared measurements above them still run
    // - the canopy reads the same numbers. ARENA FULL adds the arena's
    // verbose rows; the canopy has no such level.
    hud_panels     = (hud_style != kHudOff);
    canopy_on      = (hud_style == kHudCanopy);
    legacy_panels  = (hud_style == kHudArena || hud_style == kHudArenaFull);
    hud_verbose    = (hud_style == kHudArenaFull);
    legacy_verbose = hud_verbose;
    (void)hud_panels;

    link_copy = osd_vars.radio_link;
    pthread_mutex_unlock(&osd_mutex);
    prof::mark(prof::kSnapshot);
    if (menu_moving) {
        phase_lock_.store(true, std::memory_order_relaxed);
        signal_render(prof::kWakeAnim);
    }
    // What sits along the top: the clock strip.
    std::copy(model_hud_c, model_hud_c + 16, model_top);
    math::multiply(mvp_top, model_top, tmp);
    
    msp_osd.set_scale(s);
    // The panel layout's cell size. Full-canvas mode overrides it below, so it
    // is reset here every frame rather than once at startup.
    msp_osd.set_cell_size(0.03375f * s, 0.045f * s);
}

void OSD::frame_video(Frame& f) {
    auto& now_us = f.now_us;
    auto& vs = f.vs;
    auto& prev_vs = f.prev_vs;
    auto& trans_t = f.trans_t;
    auto& video_active = f.video_active;
    // --- BACKGROUND VIDEO / WARP-ZOOM STATE MACHINE ---
    now_us = get_time_us();
    {
        pthread_mutex_lock(&osd_mutex);
        prev_vs = video_state_; // snapshot BEFORE any changes this frame
        // State is one-way: BACKGROUND → TRANSITION → LIVE.
        // Once FPV is received we stay in LIVE and show the frozen last frame on
        // disconnect. The background video is never restarted after first FPV lock.
        // Advance transition.
        if (video_state_ == VideoState::TRANSITION) {
            if (TRANSITION_DURATION_US <= 0.0f) {
                // Zero duration means the warp is switched off. Take the branch
                // explicitly rather than dividing by it: on the frame where the
                // keyframe lands the elapsed time is also 0, and 0/0 is a NaN
                // that fails every comparison - which would park the state
                // machine in TRANSITION for good.
                trans_t = 1.0f;
                video_state_ = VideoState::LIVE;
            } else {
                trans_t = (float)(now_us - transition_start_us_) / TRANSITION_DURATION_US;
                if (trans_t >= 1.0f) {
                    trans_t = 1.0f;
                    video_state_ = VideoState::LIVE;
                }
            }
        }
        vs = video_state_;
        pthread_mutex_unlock(&osd_mutex);
    }

    // Stop bg video decode once warp-zoom completes (outside mutex — stop() blocks).
    // It is never restarted: after FPV lock the frozen last frame stays on screen.
    if (bg_video_enabled && bg_player_ &&
        prev_vs == VideoState::TRANSITION && vs == VideoState::LIVE) {
        fprintf(stderr, "[BgVideo] FPV connected — stopping background decode\n");
        bg_player_->stop();
    }

    // true only when FPV is actively displayed AND frames decoded recently —
    // VideoState latches LIVE, so without the freshness check the bottom-left
    // fps/resolution/bitrate labels freeze on stale values when video stops.
    // The stamp is the renderer's and can be newer than the time read here: then
    // it is fresh, not an age wrapped round to forever.
    {
        const uint64_t t = get_time_us(), last = last_fpv_frame_us_;
        video_active = (vs == VideoState::LIVE) && (t <= last || t - last < 1500000ULL);
    }

    // Upload / import latest bg video frame while not fully live (video path only)
    if (!video_active && bg_video_enabled && bg_player_) {
        if (bg_player_->is_drm() && canvas_->video_supported()) {
            // --- Zero-copy: the decoder's NV12 buffer taken as it is ---
            if (import_drm_frame(bg_player_, bg_ext_tex_)) bg_have_frame_ = true;
        } else {
            // --- Software frames: sws BGRA into an image ---
            if (bg_player_->upload_latest_frame(*canvas_, bg_video_tex_)) bg_have_frame_ = true;
        }
    }
}

void OSD::frame_wings(Frame& f) {
    auto& tmp = f.tmp;
    auto& model_l = f.model_l;
    auto& model_r = f.model_r;
    auto& mvp_l = f.mvp_l;
    auto& mvp_r = f.mvp_r;
    auto& hud_dx = f.hud_dx;
    auto& hud_dy = f.hud_dy;
    auto& hud_bank = f.hud_bank;
    auto& frustum_h = f.frustum_h;
    // 2/3. Left and Right Matrices (panel yaw +/-0.26 rad / ~15 deg), each with
    // the dynamic bank and translation shift. The panel's own yaw goes on
    // first and the bank after it, so the bank stays a view-plane tilt - the
    // whole HUD leans as one with the horizon, rather than each panel rolling
    // about its own yawed axis.
    {
        math::Mat4 bank_m;
        math::rotateZ(bank_m, hud_bank);

        math::Mat4 yaw_l;
        math::rotateY(yaw_l, 0.26f);
        math::multiply(model_l, yaw_l, bank_m);
        // The canopy awake: the picked blade lifts a little and the other one
        // sinks; a blade unfolding into its settings (or the right one into
        // the channel band, which takes the whole bottom) sinks out of the way
        // under them, since what unfolds is that blade.
        const float lift_l = ui_lift_[0] * 0.06f * frustum_h -
                             std::max(ui_open_[0], ui_band_) * 0.9f * frustum_h;
        const float lift_r = ui_lift_[1] * 0.06f * frustum_h -
                             std::max(ui_open_[1], ui_band_) * 0.9f * frustum_h;
        model_l[12] = hud_dx;
        model_l[13] = hud_dy + lift_l;
        math::multiply(mvp_l, model_l, tmp);

        math::Mat4 yaw_r;
        math::rotateY(yaw_r, -0.26f);
        math::multiply(model_r, yaw_r, bank_m);
        model_r[12] = hud_dx;
        model_r[13] = hud_dy + lift_r;
        math::multiply(mvp_r, model_r, tmp);
    }
}

void OSD::measure_link(Frame& f) {
    auto& canopy = f.canopy;
    auto& link_copy = f.link_copy;
    auto& seen_this_link = f.seen_this_link;
    auto& connected = f.connected;
    auto& dim = f.dim;
    auto& rung = f.rung;
    auto& snr_norm = f.snr_norm;
    auto& quality = f.quality;
    // The recording, for the canopy's REC and the arena's.
    if (DvrRecorder::instance().is_recording()) {
        if (rec_started_us == 0) rec_started_us = get_time_us();
        canopy.rec_active = true;
        canopy.rec_secs   = (unsigned)((get_time_us() - rec_started_us) / 1000000ULL);
    } else {
        rec_started_us = 0;
        canopy.rec_failed[0] = '\0';
        uint64_t at = 0;
        const std::string why = DvrRecorder::instance().last_failure(&at);
        if (!why.empty() && get_time_us() - at < 5000000ULL)
            snprintf(canopy.rec_failed, sizeof(canopy.rec_failed), "%s", why.c_str());
    }
    {
        auto pwr_text = [&](char* out, size_t n) { air_pwr_text(link_copy, out, n); };
        // Debounce the badge: the baseband's status RPC blips for a poll or two
        // while the radio reconfigures an MCS change — flashing IDLE for that
        // sub-second reads as "link dropped" when nothing was lost. Only show
        // IDLE after 3s without a good status.
        static uint64_t last_connected_ms = 0;
        uint64_t badge_now = get_time_ms();
        if (link_copy.state == 2) last_connected_ms = badge_now;
        connected = (last_connected_ms != 0 &&
                          badge_now - last_connected_ms < 3000);
        dim = connected ? 1.0f : 0.50f;
        // The bar is the MCS rung, not SNR. SNR is an input to link quality,
        // not a measure of it: the SNR a link needs rises with its MCS, so the
        // same 20 dB is comfortable at MCS5 and about to fail at MCS12, and a
        // raw-SNR bar reads identically in both. The rung is what actually
        // moves when quality degrades - a downshift IS the bitrate dropping.
        //
        // This radio does not use MCS 0..12 continuously. It walks seven rungs,
        // {1,2,5,7,8,10,12}, which is what the SET 36 adaptation policy in the
        // stock capture we replay installs (see kStockRfSetup) and what section
        // 24 found it accepts. Scaling by raw MCS/12 would make a downshift
        // 12->10 move the bar by 17%; over the rungs it is a clear seventh.
        const int kMcsRungs = (int)(sizeof(kMcsLadder) / sizeof(kMcsLadder[0]));
        rung = -1;
        for (int i = 0; i < kMcsRungs; i++)
            if (link_copy.rx_mcs_val >= kMcsLadder[i]) rung = i;   // nearest rung at or below
        // A working link never shows an empty bar: the lowest rung is 1/7, and
        // "no rung" (an MCS below the ladder, or none reported) is what reads
        // as empty.
        snr_norm = (rung < 0) ? 0.0f : (float)(rung + 1) / (float)kMcsRungs;
        // The composite, drawn as a thin second strip inside the same gauge so
        // the two can be watched against each other in real time. The blocks
        // above are what the link IS doing; this is what it is ABOUT to do.
        //
        //   margin  - SNR above the downshift threshold for the current rung.
        //             Thresholds are stock's own, byte 8..9 of the SET 36
        //             entries we replay (kStockRfSetup), read as dB x100.
        //             That reading of the scale is inferred, not confirmed -
        //             it is monotonic across all seven rungs, which is good
        //             evidence, but check it first if the strip misbehaves
        //             against the blocks.
        //   ldpc    - pre-FEC codeword errors, which rise while FEC is still
        //             repairing them, i.e. before any frame is lost. 3-8% is
        //             NORMAL (measured at 2m with zero post-FEC loss) - that is
        //             the FEC working, so health only falls above 10% and is
        //             dead at 50%.
        //
        // min() of the two: either one alone ruins the picture, and averaging
        // would let a fat SNR margin mask interference - the exact blind spot
        // raw SNR had.
        quality = 0.0f;
        if (rung >= 0) {
            float margin_db     = link_copy.snr - kMcsDownDb[rung];
            float margin_health = std::max(0.0f, std::min(1.0f, margin_db / 6.0f));
            float ldpc_health   = 1.0f - std::max(0.0f, std::min(1.0f,
                                        (link_copy.fec_error - 0.10f) / 0.40f));
            quality = std::min(margin_health, ldpc_health);
        }
        // Smoothed once, here, so the strip and the LINK figure below cannot
        // disagree - they are the same measurement shown two ways. Only while
        // connected: a dropped link would otherwise decay the average with
        // readings that mean nothing.
        static float quality_ewma = -1.0f;
        if (connected) {
            quality_ewma = (quality_ewma < 0.0f) ? quality
                                                 : 0.9f * quality_ewma + 0.1f * quality;
            quality = quality_ewma;
        }
        // Range left: what the link can still give up to hold on as the
        // signal fades - its levers and blocks in system/gnd/link_range.hpp,
        // which the native Ascent's canopy reads too.
        const gnd::LinkRange range = gnd::link_range(connected, link_copy.rx_mcs_val, link_copy.bw_step,
                                                     link_copy.air_pwr_mw, air_live_dbm());
        const float range_norm = range.norm;
        // Video link warning, for the screen-edge glow and the canopy's VIDEO
        // track, off the range left above (the track's own blocks, so the two
        // never disagree): amber at 5-7 of 12, red at 4 or fewer; or no
        // frame for 400 ms on a link that was delivering them - frozen.
        int warn = 0;
        if (connected) {
            warn = range.warning();
            const uint64_t t = get_time_us(), last = last_fpv_frame_us_;
            if (seen_this_link && t > last && t - last > 400000ULL)   // a newer stamp is no freeze
                warn = 2;
        }
        // The canopy reads the same measurements the arena panels draw rather
        // than re-deriving them, so the two layouts can never disagree.
        canopy.link_warn = warn;   // no hold: the blocks and their colour follow the link as it is
        canopy.link_up  = connected;
        canopy.range_norm = range_norm;
        canopy.quality  = quality;
        pwr_text(canopy.vtx_pwr, sizeof(canopy.vtx_pwr));
        canopy.vrx_pwr[0] = '\0';
        if (const int dbm = gnd_live_dbm(); dbm > 0) {
            const int mw = ar_pwr_mw_from_dbm(dbm);   // the VRX's own stock scale
            if (mw >= 1000) snprintf(canopy.vrx_pwr, sizeof(canopy.vrx_pwr), "%.1fW", mw / 1000.0f);
            else            snprintf(canopy.vrx_pwr, sizeof(canopy.vrx_pwr), "%dmW", mw);
        }
        // tx_freq, not rx_freq: rx_status reports a fixed 2100 MHz gear value
        // on this radio rather than the actual channel (see the RF tab's own
        // frequency row for the same substitution).
        canopy.link_freq_mhz = link_copy.tx_freq;
        canopy.link_bw_idx = link_copy.bw_step;
        canopy.link_mbps = link_copy.state == 2 ? link_copy.rx_data_rate_kbps / 1000.0f : 0.0f;
        // HUD > Debug Info: the frame our radio runs, read off its state word,
        // and the mode that length is - each of the six is its own length:
        // short (2), standard (0), long (1), at 40 MHz and at 20 or narrower.
        canopy.debug[0] = 0;
        if (hud_debug) {
            static const struct { float us; int mode; } kFrames[] = {
                { 3250.56f, 2 }, { 4233.60f, 0 }, { 5398.40f, 1 },
                { 4650.88f, 2 }, { 5633.92f, 0 }, { 7964.80f, 1 },
            };
            const float T = link_copy.frame_us;
            const int want = link_copy.frame_mode_asked;
            int mode = -1;
            for (const auto& f : kFrames) if (fabsf(T - f.us) < 2.0f) mode = f.mode;
            const char* name = mode == 2 ? "SHORT" : mode == 0 ? "STANDARD" : mode == 1 ? "LONG" : "?";
            if (T > 0.0f)
                snprintf(canopy.debug, sizeof(canopy.debug), "FRAME %.2fms %s%s", T / 1000.0f, name,
                         mode < 0 || mode == want ? "" : want == 2 ? " (ASKED SHORT)" : " (ASKED STANDARD)");
            else
                snprintf(canopy.debug, sizeof(canopy.debug), "FRAME --");
        }
    }
}

    // TX power text, shared: at MEDIUM it rides on the LINK row, at FULL it
    // sits on its own SNR row, and the two must not drift apart.
    //
    // This is the AIR VTX power - the power we commanded the air unit to
    // transmit the video at (cmd 0x22), which is what "TX power" means on an
    // FPV goggle. It is deliberately NOT the ground radio's own uplink power
    // (info.self.tx_power): that is the goggle-to-air control-link power, and
    // its adaptation loop drifts to the ceiling, which is what made the OSD
    // read 500mW while the menu said 25mW.
    //   (A) - auto: the air runs adaptive power capped at the shown level.
void OSD::air_pwr_text(const gnd::LinkStats& link_copy, char* out, size_t n) const {
        int cmd_mw = link_copy.air_pwr_mw;
        if (cmd_mw <= 0) { snprintf(out, n, "--"); return; }
        const ar_pwr_level &lv = kArPwrLevels[ar_pwr_index(cmd_mw)];
        int mw = lv.automode ? lv.mw - 1 : lv.mw;   // 501 (auto cap 500) -> 500
        // What the air unit says it transmits at (an Open8030 air radio),
        // rather than what it was set to: AUTO moves it. An air radio that
        // says nothing (the P1 Air's Open8030) while its unit reports standby
        // is at kestrel-air's standby level, 13 dBm, not the level for flight.
        if (const int dbm = air_live_dbm(); dbm > 0) mw = ar_air_mw_from_dbm(dbm);
        else if (vtx_low_power) mw = ar_air_mw_from_dbm(13);
        const char *aflag = lv.automode ? " (A)" : "";
        if (mw >= 1000) snprintf(out, n, "%.1fW%s", mw / 1000.0f, aflag);
        else            snprintf(out, n, "%dmW%s", mw, aflag);
}

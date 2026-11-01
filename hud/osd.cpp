// The OSD: its construction, the HUD's thread (run) and how often it draws.
#include "hud/osd_detail.hpp"

// ---------------------------------------------------------------------------
// Async asset prefetch.
//
// The two heaviest startup items are pure CPU/Cairo work with no GL dependency:
//   * decoding /etc/kestrel/background.png  (~260 ms for a 5.7 MB image)
//   * rendering the "KESTREL" idle title via Cairo/FreeType (~50 ms)
// They used to run on the OSD thread *after* eglInitialize() (~285 ms on Mali)
// had already returned, so the big costs were serialized.
//
// osd_prefetch_assets_async() is called once from main() before DRM/EGL
// bring-up, so the Cairo decode runs on another core while the GPU driver
// initializes. init_images() then only uploads the already-decoded pixels.
// ---------------------------------------------------------------------------
// Same idea for --menu: set before the OSD exists, read once when it starts.
static bool s_menu_at_start = false;
static int  s_menu_start_tab = -1;
void osd_set_menu_at_start(bool on) { s_menu_at_start = on; }
// Which tab --menu opens on. -1 leaves whatever the menu would pick.
void osd_set_menu_start_tab(int t) { s_menu_start_tab = t; }

OSD::OSD(const hud::Parts& parts, Settings& settings, std::shared_ptr<display::Display> dev_,
         std::unique_ptr<render::Canvas> canvas, std::unique_ptr<display::HudLayer> layer,
         int refresh_frequency_ms_, volatile bool* signal_stop, bool console_stats_) :
    settings_(settings),
    dev(dev_), 
    layer_(std::move(layer)),
    refresh_frequency_ms(refresh_frequency_ms_),
    console_stats(console_stats_),
    signal_stop(signal_stop),
    parts_(parts) {
    // What wakes the HUD's loop: a change to what it shows, each scan sample
    // and video rate, the FC's bytes when they start coming (taken at most
    // every 33 ms).
    parts_.radio.watch(&hud_wake_);
    parts_.air_radio.watch(&hud_wake_);
    parts_.encoder.watch(&hud_wake_);
    parts_.air_unit.watch(&hud_wake_);
    parts_.camera.watch(&hud_wake_);
    parts_.fc.watch(&hud_wake_);
    tid_dvr_menu_active = false;
    tid_dvr_menu = 0;
    
    canvas_ = std::move(canvas);
    link_ = std::make_shared<hud::LinkView>(link_view());
    
    pthread_mutex_init(&osd_mutex, NULL);
    dev->set_flip_hook([](void* c) { static_cast<OSD*>(c)->on_flip(); }, this);

    osd_vars.ui_scale = 1.0f;
    osd_vars.sky_exposure_us = 5000;
    osd_vars.sky_framerate = 120;
    osd_vars.slices_received = false;
    menu_ui_scale = 1.0f;
    applied_ui_scale = 1.0f;

    bg_player_ = new BgVideoPlayer(osd_asset("background.mp4").c_str(), dev);
    // Wake the OSD render loop when a new background frame is ready, but cap
    // idle redraws to ~30 fps. The whole HUD is recomposited on every wake, so
    // tracking the 60 fps cinematic loop doubled idle CPU for no real benefit —
    // 30 fps looks identical for an idle background. Menu/stats/transition
    // wakeups call signal_render() directly and are unaffected by this throttle.
    bg_player_->on_new_frame = [this]() {
        uint64_t now = get_time_us();
        if (now - last_bg_signal_us_ < 32000) return; // ~31 fps
        last_bg_signal_us_ = now;
        signal_render();
    };

    // Standby is not persisted or commanded: the air unit dictates it (auto-parks
    // to low power when disarmed/idle). The menu row reflects the air's reported
    // air_standby state, read-only.
    // The OSD keeps its own copy of dvr_screen for the menu row. It was never
    // loaded from settings, so the menu could read OFF while DvrRecorder (which
    // main() does initialise from the same key) was in screen mode - two views
    // of one setting, silently disagreeing.
    dvr_screen = settings_.get(dvr::setting::dvr_screen);
    dvr_dir_ = settings_.get(dvr::setting::dvr_dir);
    menu_brightness = settings_.get(hud::setting::brightness);
    if (dev) dev->set_display_property("brightness", menu_brightness);
    menu_dvr_source = dvr_source_now();
    show_all_adapters = true;
    bg_video_enabled = settings_.get(hud::setting::bg_video_enabled);
    link_edge_on_ = settings_.get(hud::setting::link_edge_warning);
    hud_debug = settings_.get(hud::setting::hud_debug);
    // 0=OFF 1=SMALL 2=MEDIUM 3=EXTREME. This was a bool before the intensity
    // levels existed, so migrate the stored "true"/"false" rather than letting
    // getInt() fail to parse them - stoi throws on both and would hand back the
    // default, quietly turning the feature back ON for anyone who had it off.
    {
        std::string hr = settings_.text(hud::setting::hud_reactivity);
        if (hr == "true")       hud_reactivity = 2;
        else if (hr == "false") hud_reactivity = 0;
        else                    hud_reactivity = settings_.get(hud::setting::hud_reactivity);
        if (hud_reactivity < 0 || hud_reactivity > 3) hud_reactivity = 2;
        // How far up the air unit's camera is tilted on the airframe: found each time the aircraft is
        // armed (it is level then), saved, and used to put the camera's rates into the aircraft's axes.
        msp_osd.set_cam_tilt(settings_.get(hud::setting::imu_cam_tilt_deg));
    }

    // The clock has two independent questions - WHEN it shows and WHAT FORMAT
    // it shows in - and clock_mode used to answer both with one enum
    // (OFF|12H|24H). That worked while "when" was hard-coded to idle-only; it
    // stops working the moment the answer can be "always". So clock_show now
    // owns visibility and clock_mode is format alone.
    //
    // Migration runs off the old enum: its 0 meant off, which is a visibility
    // answer, so it becomes clock_show=0 with the format left at 24H. Older
    // still is "show_clock", a plain bool - and getInt() cannot read "false"
    // (it throws and returns the default, which would switch the clock back on
    // for anyone who had turned it off), so the new key is read first.
    {
        int cm = settings_.get(hud::setting::clock_mode);
        if (cm < 0) cm = settings_.get(hud::setting::show_clock) ? 2 : 0;

        int cs = settings_.get(hud::setting::clock_show);
        if (cs < 0) cs = (cm == 0) ? 0 : 1;          // old off -> off, else idle-only
        clock_show = (cs >= 0 && cs <= 2) ? cs : 1;

        if (cm <= 0) cm = 2;                         // 0 is no longer a format
        clock_mode = (cm == 1 || cm == 2) ? cm : 2;
    }
    menu_clock_mode = clock_mode;
    menu_clock_show = clock_show;
    {
        int t = settings_.get(hud::setting::hud_theme);
        hud_theme_idx = (t >= 0 && t < hud_theme_count()) ? t : 0;
        hud_theme_set(hud_theme_idx);
    }
    menu_hud_theme = hud_theme_idx;
    menu_open_at_start = s_menu_at_start;
    if (s_menu_start_tab >= 0 && s_menu_start_tab < kMenuTabs) {
        menu_tab = s_menu_start_tab;
        menu_first_open_ = false;   // asked for by name; do not second-guess it
    }
    // HUD style and the Betaflight OSD. Both are new keys; the three they
    // replaced (hud_detail, hud_style, msp_mode) are read once when the new
    // key is absent, so a goggle comes up after the update looking as it did.
    {
        // "hud_overlay", not the first name this had ("hud_layout"): the
        // ordinals were reordered (OFF moved from 3 to 0) before this ever
        // shipped, and a goggle that had already run the first version would
        // silently misread its own old CANOPY (0) as the new OFF (also 0)
        // otherwise. A fresh name forces the same safe fallback a goggle
        // that never had either key takes.
        int hl = settings_.get(hud::setting::hud_overlay);
        if (hl < 0 || hl >= kHudStyleCount) {
            int hd = settings_.get(hud::setting::hud_detail);   // 0 MINIMAL 1 MEDIUM 2 FULL
            int hs = settings_.get(hud::setting::hud_style);    // 0 LEGACY 1 CANOPY
            if (hd == 0)      hl = kHudOff;
            else if (hs != 0) hl = kHudCanopy;
            else              hl = (hd == 2) ? kHudArenaFull : kHudArena;
        }
        hud_style = hl;
        int bf = settings_.get(hud::setting::bf_osd);
        if (bf < 0) bf = settings_.get(hud::setting::msp_mode) != 0 ? 1 : 0;
        bf_osd = (bf != 0);
        int vm = settings_.get(hud::setting::volt_mode);
        volt_mode = (vm == 0 || vm == 1) ? vm : 0;
    }
    menu_bf_osd = bf_osd;
    menu_hud_style = hud_style;
    menu_volt_mode = volt_mode;
    menu_wifi_ap = wifi_ap_on() ? 1 : 0;   // off after boot; see wifi_ap.hpp
    menu_show_all_adapters = true;
    menu_bg_video = bg_video_enabled;
    menu_frame_mode = g_frame_mode.load();
    {
        const display::RefreshSync rs = refresh_sync();
        menu_refresh_sync = rs.mode;
        menu_sync_slice = rs.slice;
        menu_sync_margin = rs.margin_us / 1000;
    }
    menu_hud_reactivity = hud_reactivity;
    menu_hud_debug = hud_debug;

    // Restore picture size. 100 is the untouched full-screen path.
    int saved_scale = settings_.get(hud::setting::picture_scale);
    if (saved_scale >= 60 && saved_scale < 100) {
        menu_picture_scale = saved_scale;
        dev->set_picture_scale(saved_scale);
    }

    menu_zone_ = zones::selected();
    menu_zone_gen_ = zones::generation();
}

void OSD::on_flip() {
    last_flip_seen_us_.store(get_time_us(), std::memory_order_relaxed);
    if (!phase_lock_.load(std::memory_order_relaxed)) return;
    flip_tick_ = true;
    hud_wake_.poke();
}

OSD::~OSD() {
    dev->set_flip_hook(nullptr, nullptr);
    canvas_->bind_thread();   // the images go here, what it draws with with the canvas
    canvas_->free_image(fps_tex);
    canvas_->free_image(lat_tex);
    canvas_->free_image(net_tex);
    canvas_->free_image(logo_tex);
    canvas_->free_image(bg_tex);
    canvas_->free_image(bg_video_tex_);
    canvas_->free_image(bg_ext_tex_);
    clip_player_.reset();    // joins its decoder before the images go
    canvas_->free_image(clip_ext_tex_);
    canvas_->free_image(live_ext_tex_);
    canvas_->free_image(gallery_snap_tex_);
    for (auto& th : gallery_thumbs_) canvas_->free_image(th.second.tex);
    if (bg_player_) { bg_player_->stop(); delete bg_player_; bg_player_ = nullptr; }

    // /tmp/kestrel-adapt.csv was lazy-opened and never closed, so the last rows
    // of a run were lost in stdio's buffer on exit.
    if (csv_fp) { fclose((FILE *)csv_fp); csv_fp = nullptr; }

    if (dvr) {
        dvr->stop();
        if (tid_dvr_menu_active) {
            pthread_join(tid_dvr_menu, NULL);
            tid_dvr_menu_active = false;
        }
        dvr = nullptr;
    }

    pthread_mutex_destroy(&osd_mutex);
}

void OSD::signal_render(prof::Wake why) {
    prof::wake(why);
    // A video frame alone is no reason to redraw: the video has its own plane
    // and the HUD is not redrawn to show it. This used to render the HUD once
    // per video frame, 60-120 times a second, mostly to identical pictures.
    // notify_video_frame() wakes the loop on the video edges that do change
    // the HUD (first keyframe, resume after a stall, video confirmed).
    if (why == prof::kWakeVideo) return;

    if (why == prof::kWakeAnim) {
        // From inside a frame: another frame is wanted, one refresh from now,
        // not the instant this one finishes.
        pthread_mutex_lock(&osd_mutex);
        anim_pending_ = true;
        pthread_mutex_unlock(&osd_mutex);
    } else {
        render_requested = true;      // the radio's wakes: no lock
        hud_wake_.poke();
    }
}

void OSD::set_ui_scale(float v) {
    pthread_mutex_lock(&osd_mutex);
    osd_vars.ui_scale = v;
    menu_ui_scale = v;
    applied_ui_scale = v;
    pthread_mutex_unlock(&osd_mutex);
}

void OSD::run() {
    SchedulingHelper::configure_thread(SchedulingHelper::ThreadRole::OSD);

    canvas_->bind_thread();
    init_images();
    canvas_->init();

    printf("OSD::run GL start (Handled on video frame update).\n");

    // Paint the idle screen immediately instead of waiting for the first
    // background-video frame (or the 1 s refresh timeout) — gets pixels on the
    // display ~200 ms sooner at cold start.
    if (layer_ready_) render_frame();

    // Render pacing. The HUD is redrawn when something on it changes, at a
    // rate that fits what is changing, and never just because a video frame
    // arrived (the video is on its own plane):
    //   - an animation in progress asks for its next frame 16.7 ms out
    //   - input, link events and video edges are drawn promptly, but never
    //     closer than 16 ms to the previous frame (bursts coalesce)
    //   - new MSP data is parsed, and drawn only if it changed the HUD
    //   - otherwise a steady refresh: see next_render_interval_us()
    // Each frame costs a full-screen GPU pass, and on battery the frames not
    // drawn are the saving.
    constexpr uint64_t kMinGapUs = 16000;
    while(!*signal_stop) {
        uint64_t interval = next_render_interval_us();
        // Phase-locked to the display's flips while the gallery animates: a flip starts the
        // next frame, and the timer is only the fallback for when none comes (video stalled).
        const bool lock_flips = phase_lock_.load(std::memory_order_relaxed);
        if (lock_flips && interval < 30000) interval = 30000;
        uint64_t due = loop_start_us_ + interval;

        pthread_mutex_lock(&osd_mutex);
        // The FC's bytes are taken at most every 33 ms: waiting, they are due
        // one throttle period after the last take.
        auto wake_at = [&] {
            return parts_.fc.pending() ? std::min<uint64_t>(due, last_msp_signal_us_ + 33000) : due;
        };
        sync_radio();   // what the radio published since (an event may ask for a frame)
        uint64_t now = get_time_us();
        // Sleep on the waker, without the lock: every wake reason is a flag
        // (or queued data) set before its poke, and a poke that comes before
        // the sleep is kept.
        for (;;) {
            if (render_requested || radio_changed() || (lock_flips && flip_tick_ && anim_pending_) || *signal_stop)
                break;
            const uint64_t until = wake_at();    // under the lock: what the sleep below waits for
            if (now >= until) break;
            pthread_mutex_unlock(&osd_mutex);
            hud_wake_.wait(until - now);         // > 0: a timeout, never "forever"
            pthread_mutex_lock(&osd_mutex);
            sync_radio();
            now = get_time_us();
        }
        now = get_time_us();
        bool requested = render_requested.exchange(false);
        if (radio_changed()) {   // the radio or the air unit moved: as signal_render(kWakeLink)
            radio_drawn_ = radio_synced_;
            air_drawn_ = air_synced_;
            prof::wake(prof::kWakeLink);
            requested = true;
        }
        const bool msp = parts_.fc.pending() && now >= last_msp_signal_us_ + 33000;
        const bool tick = lock_flips && flip_tick_ && anim_pending_;
        bool heartbeat = now >= due || tick;
        flip_tick_ = false;
        if (msp) last_msp_signal_us_ = now;
        pthread_mutex_unlock(&osd_mutex);

        if (*signal_stop) break;

        // Parse whatever the RX thread staged since the last repaint, so the
        // grid is current before it is drawn.
        drain_msp();
        uint32_t msp_ver = msp_osd.content_version();
        if (!requested && !heartbeat && msp_ver == last_msp_ver_)
            continue;   // MSP arrived, nothing the HUD shows changed
        bool timed_out = heartbeat && !requested;

        if (timed_out) prof::wake(prof::kWakeTimeout);
        else if (!requested) prof::wake(prof::kWakeOther);   // MSP change

        now = get_time_us();
        // Locked to the flips, the flip decides when a frame starts: the usual 16 ms minimum
        // is shorter than a 60 fps flip cycle, so any frame started a little late would set
        // the next by the clock and the phase would slide off the flips for good.
        const uint64_t min_gap = lock_flips ? 12000 : kMinGapUs;
        if (now - loop_start_us_ < min_gap) usleep((useconds_t)(min_gap - (now - loop_start_us_)));

        last_msp_ver_ = msp_ver;
        pthread_mutex_lock(&osd_mutex);
        anim_pending_ = false;   // this frame answers it; render_frame asks again if still animating
        pthread_mutex_unlock(&osd_mutex);
        loop_start_us_ = get_time_us();
        if (layer_ready_) render_frame();
    }
}

void OSD::test_init() {
    canvas_->bind_thread();
    init_images();
    canvas_->init();
}

void OSD::test_frame() {
    pthread_mutex_lock(&osd_mutex);
    sync_radio();
    radio_drawn_ = radio_synced_;
    air_drawn_ = air_synced_;
    anim_pending_ = false;
    pthread_mutex_unlock(&osd_mutex);
    drain_msp();
    last_msp_ver_ = msp_osd.content_version();
    loop_start_us_ = get_time_us();
    if (layer_ready_) render_frame();
}

// How long the loop may go without drawing when nothing asks it to.
uint64_t OSD::next_render_interval_us() {
    pthread_mutex_lock(&osd_mutex);
    bool anim = anim_pending_;
    pthread_mutex_unlock(&osd_mutex);
    if (anim) return 16667;                                  // animating: 60 Hz

    uint64_t us = (uint64_t)refresh_frequency_ms * 1000;    // idle: 1 s
    // (The HUD moving with the craft asks for frames itself while its spring
    // is settling - see render_frame.)
    // Connected: figures, freshness and stall detection (the red rails come
    // up 400 ms into a stall) - 10 Hz is ample for all of them.
    if (link_watch_ || hud_connected_)
        us = std::min<uint64_t>(us, 100000);
    return us;
}


void OSD::set_dvr(std::shared_ptr<DVR> dvr_, bool dvr_screen_) {
    pthread_mutex_lock(&osd_mutex);
    dvr = dvr_;
    dvr_screen = dvr_screen_;
    pthread_mutex_unlock(&osd_mutex);
}

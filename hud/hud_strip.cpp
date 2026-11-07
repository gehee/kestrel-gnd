// What every HUD style shares: the shade behind it, the strip along the top
// (the clock, REC) - which also settles whether there is an aircraft - and
// the edge glowing as the video link weakens.
#include "hud/osd_detail.hpp"

void OSD::draw_shade(Frame& f) {
    auto& mvp_hud_c = f.mvp_hud_c;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& bg_r = f.bg_r;
    auto& bg_g = f.bg_g;
    auto& bg_b = f.bg_b;
    // --- GLOBAL BACKGROUND SHADE ---
    // A large, extremely subtle hexagonal shade behind everything
    canvas_->set_transform(mvp_hud_c);
    canvas_->hex_panel(-frustum_w, -frustum_h, frustum_w * 2.0f, frustum_h * 2.0f, 0.5f, bg_r, bg_g, bg_b, false);
}

void OSD::draw_top_strip(Frame& f) {
    auto& mvp_top = f.mvp_top;
    auto& frustum_h = f.frustum_h;
    auto& s = f.s;
    auto& canopy_on = f.canopy_on;
    auto& legacy_panels = f.legacy_panels;
    auto& link_copy = f.link_copy;
    auto& video_active = f.video_active;
    // --- MENU RENDERING ---
    // --- TOP CENTRE: distance, clock, temperature ---
    // These three sit above the video rather than in the RF panel because they
    // are wanted at every detail level - including MINIMAL, where the panels
    // are gone entirely and the Betaflight OSD has the screen. Distance and
    // temperature are the two readings a pilot acts on in the air; the rest of
    // the RF panel is diagnostics.
    //
    // Laid out as one centred group so the clock does not shift as the strings
    // beside it change width.
    {
        float clk_scale    = 0.075f * s;
        float hud_margin_t = 0.02f;
        float clk_y        = frustum_h - hud_margin_t - clk_scale;
        float gap          = clk_scale * 0.55f;
        float icon_sz      = clk_scale;
        float icon_gap     = clk_scale * 0.35f;

        char clock_buf[16] = {0};
        if (clock_show != 0) {
            time_t     now_t = time(nullptr);
            struct tm  tm_now;
            if (localtime_r(&now_t, &tm_now)) {
                if (clock_mode == 2) {
                    // 24-hour pads the hour ("09:11"); 12-hour does not
                    // ("9:11 AM"). That is the convention each form is read with.
                    snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
                } else {
                    int h12 = tm_now.tm_hour % 12;
                    if (h12 == 0) h12 = 12;          // midnight and noon are 12, not 0
                    snprintf(clock_buf, sizeof(clock_buf), "%d:%02d %s", h12, tm_now.tm_min,
                             tm_now.tm_hour < 12 ? "AM" : "PM");
                }
            }
        }

        // Temperature. VRX is this unit's RK3568 (/sys/class/thermal); VTX is
        // the air unit's SoC from sky cmd 0x05 byte 1. Thresholds are this
        // board's own judgement: RK3568 designs commonly throttle at 85-95C,
        // so 80C leaves margin; 90C for the VTX is a placeholder pending a
        // real stock threshold.
        static float    vrx_temp_c  = 0.0f;
        static uint64_t vrx_temp_ms = 0;

        // This box's own supply voltage, from the ADC channel, divider and
        // averaging its BSP gives (bsp/vrx_pro.cpp has where the VRX Pro's
        // come from). It is NOT on the baseband, despite the AR8030 having an
        // ADC - that channel is an NTC thermistor stock uses to drive the fan.
        //
        //   pin_mV    = raw * ref_mv / full_raw
        //   avg       = mean of the last `average` pin_mV samples
        //   supply_mV = avg * divider - offset_mv
        //
        // Stock samples about every 400ms and discards the first conversion
        // of each cycle; both are copied here because a steep divider turns
        // one bad count into many millivolts.
        const bsp::SupplyAdc &supply = bsp::board().supply;
        const int vavg = supply.average > 0 && supply.average <= 16 ? supply.average : 1;
        static float    vrx_volts   = 0.0f;
        static uint64_t vrx_volts_ms = 0;
        static int      vbuf[16] = {0};
        static int      vcount = 0, vhead = 0;
        uint64_t temp_now_ms = get_time_ms();
        if (temp_now_ms - vrx_temp_ms > 2000) {   // sysfs read - not every frame
            vrx_temp_ms = temp_now_ms;
            FILE *tf = fopen(bsp::board().thermal_zone, "r");
            if (tf) {
                int millideg = 0;
                if (fscanf(tf, "%d", &millideg) == 1) vrx_temp_c = millideg / 1000.0f;
                vrx_temp_cached_ = vrx_temp_c;
                fclose(tf);
            }
        }
        if (temp_now_ms - vrx_volts_ms > 400 && *supply.adc.path) {
            vrx_volts_ms = temp_now_ms;
            FILE *vf = fopen(supply.adc.path, "r");
            if (vf) {
                int raw = -1, first = -1;
                // Discard the first conversion, as stock does.
                if (fscanf(vf, "%d", &first) == 1) {
                    rewind(vf);
                    if (fscanf(vf, "%d", &raw) != 1) raw = first;
                }
                fclose(vf);
                if (raw >= 0) {
                    int pin_mv = bsp::adc_mv(supply.adc, raw);
                    vbuf[vhead] = pin_mv;
                    vhead = (vhead + 1) % vavg;
                    if (vcount < vavg) vcount++;
                    int sum = 0;
                    for (int i = 0; i < vcount; i++) sum += vbuf[i];
                    int avg = sum / vcount;          // live from the first sample
                    int batt_mv = bsp::supply_mv(supply, avg);
                    vrx_volts = batt_mv > 0 ? batt_mv / 1000.0f : 0.0f;
                }
            }
        }
        vrx_volts_cached_ = vrx_volts;
        // The strip is the IDLE state, not a permanent status bar. Distance
        // and both temperatures moved into the panels, where they sit beside
        // the thing they describe; what is left here says only that there is
        // nothing to fly yet. Once there is, the bar goes away entirely -
        // knowing the time is worth a line of the screen when nothing is
        // happening, and worth none of it when something is.
        static uint64_t last_conn_ms = 0;
        uint64_t strip_ms = get_time_ms();
        if (link_copy.state == 2) last_conn_ms = strip_ms;
        BfTelem strip_bf = msp_osd.get_telem();
        // Evidence that there is an aircraft, from three independent sources: a
        // radio status of 2, frames arriving, or a flight controller talking.
        // Any one of them is proof enough - the AR8030 does fail to reach state
        // 2 with video on screen, and gating on it alone flagged idle over a
        // HUD full of live readings.
        bool raw_aircraft = (last_conn_ms && strip_ms - last_conn_ms < 3000) ||
                            video_active ||
                            (strip_bf.stamp_us && get_time_us() - strip_bf.stamp_us < 3000000ULL);

        // The hold belongs on the way OUT, not on the way in.
        //
        // Holding the rising edge was worse than the flicker it guarded
        // against: for the length of the hold the link is genuinely back, so
        // video is flowing and the readings are live, but the HUD is still
        // calling itself idle - and then it tears the panels down and rebuilds
        // them on top of a picture that was already working. You watched the
        // OSD come alive and then reassemble itself.
        //
        // Debouncing the drop instead gets the same protection for free. A link
        // that blinks never leaves CONNECTED at all, so there is no rebuild to
        // suppress; only a drop long enough to be a real dropout reaches IDLE_2,
        // and coming back from that fires the rebuild on the first frame of
        // evidence, with nothing shown before it.
        uint64_t strip_us = get_time_us();
        if (raw_aircraft) {
            hud_raw_down_us_ = 0;
            if (!hud_connected_) {
                hud_connected_since_us_ = strip_us;
                frames_since_connect_   = 0;
                lock_hold_until_us_     = strip_us + kLockMinUs;
            }
            hud_connected_   = true;
        } else if (hud_connected_) {
            if (!hud_raw_down_us_) hud_raw_down_us_ = strip_us;
            if (strip_us - hud_raw_down_us_ >= kLinkDropHoldUs) hud_connected_ = false;
            // Nothing else need be driving repaints while the link is gone, and
            // the drop has to land on time.
            else signal_render(prof::kWakeAnim);
        }
        bool aircraft_here = hud_connected_;

        // IDLE ONLY (1) gives the line back to the video the moment there is
        // something to fly; ON (2) keeps the time on screen throughout. The
        // IDLE flag itself belongs to the state, not to the setting, so it
        // appears whenever there is no aircraft and never once there is - the
        // canopy's own IDLE label says the same thing at the same moment.
        // IDLE ONLY (1) gives the line back to the video the moment there is
        // something to fly; ON (2) keeps the time on screen throughout.
        //
        // The strip carries no IDLE word any more. Before the first link there
        // are no panels on screen, and an empty frame with a clock in it says
        // "nothing to fly" more plainly than a label does; after a dropout the
        // canopy's own IDLE sits on the right panel's rail, where it is next to
        // the readings it is qualifying. The word was only ever needed when it
        // was the sole indication, and it no longer is.
        // The recording indicator has to live here too, not only in the canopy.
        // The canopy is not drawn at all before the first link, so pressing REC
        // on an idle goggle started a recording and showed nothing whatsoever -
        // the only way to know it was running was to look for the file.
        char rec_buf[32]; rec_buf[0] = '\0';
        // Only when no panel is already saying it. The canopy carries its own
        // REC on the left blade and the legacy HUD has one too, so the strip is
        // the fallback for the one case neither covers: IDLE_1, where the
        // canopy has never drawn. In IDLE_2 the blades are back - dimmed, but
        // drawn - and two indicators for one state is worse than none, because
        // the second one invites you to wonder what it means that is different.
        const bool panel_has_rec = (canopy_on && canopy_link_up_) || legacy_panels;
        bool rec_now = DvrRecorder::instance().is_recording() && !panel_has_rec;
        if (rec_now) {
            if (rec_started_us == 0) rec_started_us = get_time_us();
            unsigned rs = (unsigned)((get_time_us() - rec_started_us) / 1000000ULL);
            snprintf(rec_buf, sizeof(rec_buf), "REC %u:%02u", rs / 60, rs % 60);
        } else if (!panel_has_rec) {
            // REC gone off by itself, the card no longer taking it: why, where
            // it was, for 5 s (drawn before measure_link has it).
            uint64_t at = 0;
            const std::string why = DvrRecorder::instance().last_failure(&at);
            if (!why.empty() && get_time_us() - at < 5000000ULL) {
                rec_now = true;
                snprintf(rec_buf, sizeof(rec_buf), "%s", why.c_str());
            }
        }
        const bool want_clock = clock_buf[0] && (clock_show == 2 || !aircraft_here);

        // Recording shows the strip even with the clock switched off: the
        // setting is about wanting the time on screen, not about hiding that
        // the goggle is writing to the card.
        if (want_clock || rec_now) {
            const float clk_w = want_clock ? canvas_->text_width(clock_buf, clk_scale) : 0.0f;
            const float sep_w = (want_clock && rec_now) ? canvas_->text_width("\xE2\x80\xA2", clk_scale) : 0.0f;
            const float rec_w = rec_now ? canvas_->text_width(rec_buf, clk_scale) : 0.0f;
            const float dot_w = rec_now ? icon_sz * 0.45f : 0.0f;

            float total = 0.0f;
            if (want_clock) total += icon_sz + icon_gap + clk_w;
            if (want_clock && rec_now) total += gap + sep_w + gap;
            if (rec_now) total += dot_w + icon_gap + rec_w;

            float x = -total * 0.5f;
            canvas_->set_transform(mvp_top);
            if (want_clock) {
                canvas_->icon(clock_tex, x, clk_y, icon_sz, icon_sz, 1.0f, 1.0f, 1.0f);
                x += icon_sz + icon_gap;
                canvas_->text(clock_buf, x, clk_y, clk_scale, false, 1.0f, 1.0f, 1.0f);
                x += clk_w;
            }
            if (want_clock && rec_now) {
                x += gap;
                canvas_->text("\xE2\x80\xA2", x, clk_y, clk_scale, false, 0.45f, 0.5f, 0.55f);
                x += sep_w + gap;
            }
            if (rec_now) {
                // The same red the canopy's indicator uses, and a dot rather
                // than a word so it reads as a state at a glance.
                canvas_->panel(x, clk_y + clk_scale * 0.30f, dot_w, dot_w, 1.0f, 1.0f, 0.25f, 0.25f);
                x += dot_w + icon_gap;
                canvas_->text(rec_buf, x, clk_y, clk_scale, false, 1.0f, 0.35f, 0.35f);
            }
        }

    }
}

void OSD::draw_link_edge(Frame& f) {
    auto& mvp_c = f.mvp_c;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& canopy = f.canopy;
    // Video link warning: rails along the picture's top and bottom edges glow
    // amber, then red, as the link degrades - where the eye already is, without reading anything.
    // Fades in fast and out slowly; red breathes so it cannot be mistaken
    // for a tint in the footage.
    {
        uint64_t t = get_time_us();
        float dt = link_edge_ts_ ? (float)(t - link_edge_ts_) / 1e6f : 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        // At rest (fully off or fully on) the loop draws at 10 Hz, so the time
        // since the last frame is no measure of the fade: start it with one
        // animation frame, not a 100 ms jump.
        if ((link_edge_a_ <= 0.001f || link_edge_a_ >= 0.999f) && dt > 1.0f / 60.0f) dt = 1.0f / 60.0f;
        link_edge_ts_ = t;
        int lvl = link_edge_on_ ? canopy.link_warn : 0;
        if (lvl > 0) link_edge_shown_ = lvl;
        float target = lvl > 0 ? 1.0f : 0.0f;
        float rate   = (target > link_edge_a_) ? 1.0f / 0.15f : 1.0f / 0.60f;
        float step   = rate * dt;
        if (link_edge_a_ < target) link_edge_a_ = fminf(target, link_edge_a_ + step);
        else                       link_edge_a_ = fmaxf(target, link_edge_a_ - step);

        if (link_edge_a_ > 0.001f) {
            float a = link_edge_a_, r, g, b;
            if (link_edge_shown_ >= 2) {
                r = 1.0f; g = 0.30f; b = 0.25f;
                a *= 0.65f + 0.35f * (0.5f + 0.5f * sinf((float)(t % 10000000ULL) / 1e6f * 7.54f));
            } else {
                r = 1.0f; g = 0.72f; b = 0.15f;
            }
            canvas_->set_transform(mvp_c);
            // 15% of the picture's height deep, at 75% strength.
            canvas_->rail_glow(frustum_w, frustum_h, 0.30f * frustum_h, 0.75f * a, r, g, b);
        }
        link_watch_ = canopy.link_up;
        // Fading in or out, or the red pulse: keep frames coming.
        if ((link_edge_a_ > 0.001f && link_edge_a_ < 0.999f) ||
            (link_edge_a_ > 0.001f && link_edge_shown_ >= 2))
            signal_render(prof::kWakeAnim);
    }
}

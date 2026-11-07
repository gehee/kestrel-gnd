// The ARENA HUD: a panel on each side, latency and the video on the left, the
// link on the right (HUD > HUD Style ARENA, ARENA FULL).
#include "hud/osd_detail.hpp"

void OSD::draw_arena_left(Frame& f) {
    auto& mvp_l = f.mvp_l;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& hud_margin_l = f.hud_margin_l;
    auto& hud_margin_r = f.hud_margin_r;
    auto& last_latency_frame = f.last_latency_frame;
    auto& have_latency_frame = f.have_latency_frame;
    auto& link_stats_copy = f.link_stats_copy;
    auto& v_width = f.v_width;
    auto& v_height = f.v_height;
    auto& cur_fps = f.cur_fps;
    auto& v_bw = f.v_bw;
    auto& lat_avg = f.lat_avg;
    auto& lat_max = f.lat_max;
    auto& s = f.s;
    auto& legacy_panels = f.legacy_panels;
    auto& legacy_verbose = f.legacy_verbose;
    auto& canopy = f.canopy;
    auto& cyan_r = f.cyan_r;
    auto& cyan_g = f.cyan_g;
    auto& cyan_b = f.cyan_b;
    auto& bg_r = f.bg_r;
    auto& bg_g = f.bg_g;
    auto& bg_b = f.bg_b;
    auto& video_active = f.video_active;
    auto& panel_width = f.panel_width;
    auto& panel_height = f.panel_height;
    auto& slant_offset_x = f.slant_offset_x;
    auto& slant_offset_y = f.slant_offset_y;
    auto& bx = f.bx;
    auto& rx = f.rx;
    auto& bg_bx = f.bg_bx;
    auto& bg_rx = f.bg_rx;
    auto& panel_bottom_y = f.panel_bottom_y;
    auto& content_margin = f.content_margin;
    auto& bar_h_offset = f.bar_h_offset;
    char buf[128];
    // --- PANEL LAYOUT VARS ---
    panel_width = 1.2f * s;
    panel_height = 0.90f * s;
    slant_offset_x = 0.30f; 
    slant_offset_y = 0.15f; 
    
    bx = -frustum_w + hud_margin_l + slant_offset_x; 
    rx = frustum_w - hud_margin_r - panel_width - slant_offset_x;
    
    bg_bx = -frustum_w; 
    bg_rx = frustum_w - panel_width - 0.05f * s; 
    panel_bottom_y = -frustum_h; 
    content_margin = 2.0f * hud_margin_l;
    
    bar_h_offset = 0.02f * s + 0.02f * s; 
    float current_ly = -frustum_h + content_margin + slant_offset_y + bar_h_offset;

    // --- BOTTOM LEFT: Health-Bar Latency & Stats ---
    canvas_->set_transform(mvp_l);
    // 0.95, not 0.8: over a bright FPV feed the wing panels washed out. Their
    // gradient still fades towards the screen edge, so this raises the strong
    // end rather than flattening the look.
    if (legacy_panels)
        canvas_->hex_panel(bg_bx, panel_bottom_y, panel_width, panel_height, 0.95f, bg_r, bg_g, bg_b, true);
    
    float data_mbps = video_active ? v_bw : 0.0f;
    int mcs = (!link_stats_copy.antennas.empty()) ? link_stats_copy.antennas[0].mcs_index : 0;

    // (Removed) A "fake placeholder" here substituted 15 Mbps / MCS5 when the
    // real values were unavailable. It rendered invented numbers in the same
    // style as measurements, which is worse than showing nothing.

    float max_lat = 50.0f;
    // Zero until a real frame supplies a breakdown. These used to default to
    // 5/21/8/10ms - a plausible-looking invented profile that summed to the
    // invented 44ms "max" below, so the bar drew a convincing shape from
    // nothing.
    float r_proc = 0.0f, r_net = 0.0f,
          r_dec = 0.0f, r_disp = 0.0f;
    
    if (have_latency_frame) {
        const auto& last_frame = last_latency_frame;
        float total = last_frame.processing_ms + last_frame.net_ms + last_frame.dec_ms + last_frame.disp_ms;
        if (total > 0.1f) {
            r_proc = last_frame.processing_ms / max_lat;
            r_net = last_frame.net_ms / max_lat;
            r_dec = last_frame.dec_ms / max_lat;
            r_disp = last_frame.disp_ms / max_lat;
        }
    }

    float bar_h = 0.04f * s;
    float m_slant = 0.0f; 

    // (Removed) lat_avg/lat_max used to be forced to 30.0/44.0 whenever the
    // real measurement read 0 while video was active - fabricated numbers
    // presented identically to measured ones. Now 0 stays 0 and the row
    // renders "--", so "no measurement" is visibly different from "12.3ms".

    // Left panel dim factor: grey when no active video (mirrors right panel behaviour)
    float ldim = video_active ? 1.0f : 0.5f;

    if (legacy_panels)
    canvas_->stacked_bar(bx + hud_margin_l, current_ly, 0.85f * s, bar_h, m_slant, false,
                     r_proc, cyan_r * ldim, cyan_g * ldim, cyan_b * ldim,
                     r_net, 0.012f * ldim, 0.808f * ldim, 0.643f * ldim, // Teal for RF
                     r_dec, 0.918f * ldim, 0.769f * ldim, 0.208f * ldim, // Yellow for Dec
                     r_disp, 0.9f * ldim, 0.2f * ldim, 0.2f * ldim,
                     0.0f, 0.0f, 0.0f, 0.0f,                             // (a fifth and sixth part: none)
                     0.0f, 0.0f, 0.0f, 0.0f);

    current_ly += 0.07f * s;
    // With the air delay measured (the AR8030 path: see air_delay_for) this is
    // the slowest slice's, from its first row's capture to the scan-out lighting
    // that row (Renderer::update_stats). Without it, arrival to vblank only,
    // and labelled so.
    if (video_active && lat_avg > 0.0f) {
        sprintf(buf, !air_delay_valid ? "LATENCY (gnd): %.1fms | MAX: %.1fms"
                                      : "LATENCY (to scan-out): %.1fms | MAX: %.1fms",
                lat_avg, lat_max);
    } else if (video_active) {
        sprintf(buf, "LATENCY (gnd): -- | MAX: --");
    } else {
        sprintf(buf, "LATENCY (gnd): 0.0ms | MAX: 0.0ms");
    }
    if (legacy_panels) {
        canvas_->text(buf, bx + hud_margin_l, current_ly, 0.07f * s, false, ldim, ldim, ldim);
        current_ly += 0.085f * s;
    }

    if (legacy_verbose) {
    if (video_active) {
        // Only components with a live producer. cap/enc/net came from the
        // kestrel-air RTP headers and have read a hard 0 since that pipeline
        // was removed - printing them implied a measurement that no longer
        // exists. The air unit's own encode/RF timings are not recoverable
        // from the AR8030 link: the baseband SDK exposes no delay field, the
        // sky telemetry frame (cmd 0x05) carries none, and stock's own debug
        // OSD shows its sky delay triple as "S: 0,0,0" on this hardware - so
        // stock does not have them here either.
        float dec_avg  = osd_vars.decoding_latency_avg;
        float disp_avg = osd_vars.display_latency_avg;
        if (air_delay_valid)
            // "air" is capture stamp to the first slice's arrival: camera
            // readout, encode, the air app, the radio and the goggle's daemon.
            sprintf(buf, "[air:%.1f, dec:%.1f, disp:%.1f]",   // dec includes the slices' arrival
                    air_delay_ms.load(), dec_avg, disp_avg);
        else
            sprintf(buf, "[dec:%.1f, disp:%.1f]  (ground)", dec_avg, disp_avg);
    } else {
        sprintf(buf, "[dec:0.0, disp:0.0]  (ground)");
    }
    canvas_->text(buf, bx + hud_margin_l, current_ly, 0.058f * s, false, ldim * 0.8f, ldim * 0.8f, ldim * 0.8f);
    current_ly += 0.09f * s;
    }   // hud_verbose

    // Kept at MEDIUM: what the link is actually delivering. The greyed
    // per-stage breakdown above is diagnostics and stays FULL-only.
    if (legacy_panels) {
        if (video_active) {
            sprintf(buf, "%dx%d | %d FPS | %.1f Mbps", v_width, v_height, cur_fps, data_mbps);
        } else {
            sprintf(buf, "- | 0 FPS | 0.0 Mbps");
        }
        canvas_->text(buf, bx + hud_margin_l, current_ly, 0.07f * s, false, ldim, ldim, ldim);
        current_ly += 0.09f * s;
    }

    // Recording indicator: a red dot plus elapsed time, sitting directly above
    // the resolution/fps/bitrate line. current_ly grows upward, so drawing it
    // after that line's advance puts it above on screen. Driven straight off
    // DvrRecorder, which owns the writer's lifetime.
    if (canopy.rec_active || canopy.rec_failed[0]) {   // measure_link
        const unsigned secs = canopy.rec_secs;
        float dot_x = bx + hud_margin_l;
        if (canopy.rec_active) sprintf(buf, "   REC  %u:%02u", secs / 60, secs % 60);
        else snprintf(buf, sizeof(buf), "   %s", canopy.rec_failed);   // why it went off by itself
        if (legacy_panels) {
            canvas_->poly({{dot_x, current_ly + 0.012f},
                       {dot_x + 0.018f * s, current_ly + 0.012f},
                       {dot_x + 0.018f * s, current_ly + 0.030f},
                       {dot_x, current_ly + 0.030f}}, 1.0f, 0.95f, 0.15f, 0.15f);
            canvas_->text(buf, dot_x, current_ly, 0.07f * s, false, 1.0f, 0.35f, 0.35f);
        }
        current_ly += 0.09f * s;
    }
}

void OSD::draw_arena_right(Frame& f) {
    auto& mvp_r = f.mvp_r;
    auto& frustum_h = f.frustum_h;
    auto& hud_margin_r = f.hud_margin_r;
    auto& s = f.s;
    auto& legacy_panels = f.legacy_panels;
    auto& hud_verbose = f.hud_verbose;
    auto& link_copy = f.link_copy;
    auto& cyan_r = f.cyan_r;
    auto& cyan_g = f.cyan_g;
    auto& cyan_b = f.cyan_b;
    auto& neon_r = f.neon_r;
    auto& neon_g = f.neon_g;
    auto& neon_b = f.neon_b;
    auto& bg_r = f.bg_r;
    auto& bg_g = f.bg_g;
    auto& bg_b = f.bg_b;
    auto& panel_width = f.panel_width;
    auto& panel_height = f.panel_height;
    auto& slant_offset_y = f.slant_offset_y;
    auto& rx = f.rx;
    auto& bg_rx = f.bg_rx;
    auto& panel_bottom_y = f.panel_bottom_y;
    auto& content_margin = f.content_margin;
    auto& bar_h_offset = f.bar_h_offset;
    auto& connected = f.connected;
    auto& dim = f.dim;
    auto& rung = f.rung;
    auto& snr_norm = f.snr_norm;
    auto& quality = f.quality;
    // MSP OSD's corner-panel render (the old "PANELS" mode) lived here, split
    // across this hex panel and its counterpart on the left. Removed along with
    // PANELS - MSP OSD is now ON/OFF, always the full canvas (draw_betaflight).
    char buf[128];
    auto pwr_text = [&](char* out, size_t n) { air_pwr_text(link_copy, out, n); };
    float current_ry = -frustum_h + content_margin + slant_offset_y + bar_h_offset;
    float outer_x = rx + panel_width - hud_margin_r; 
    float bar_x = outer_x - 0.85f * s; 

    // --- BOTTOM RIGHT: AR803x RF Link Stats ---
    canvas_->set_transform(mvp_r);
    if (legacy_panels)
        canvas_->hex_panel(bg_rx, panel_bottom_y, panel_width, panel_height, 0.95f, bg_r, bg_g, bg_b, true);

    {
        float total_gauge_h = 0.045f * s;
        float rs = 0.085f * s; // row step


        auto get_snr_color = [&](float val) -> std::array<float, 3> {
            if (val > 0.6f) return {cyan_r, cyan_g, cyan_b};
            if (val > 0.35f) return {1.0f, 0.8f, 0.0f};
            return {0.9f, 0.2f, 0.2f};
        };


        if (legacy_panels) {
        // The gauge is split: MCS rung on top, composite as a thin strip under
        // it. Same segment width, so both read as one gauge rather than two.
        float h_thin = total_gauge_h * 0.26f;
        float h_gap  = total_gauge_h * 0.14f;
        float h_main = total_gauge_h - h_thin - h_gap;
        if (connected) {
            auto snr_rgb = get_snr_color(snr_norm);
            canvas_->stacked_bar(bar_x, current_ry + h_thin + h_gap, 0.85f * s, h_main, 0.0f, true,
                             snr_norm, snr_rgb[0] * dim, snr_rgb[1] * dim, snr_rgb[2] * dim);
            // The theme's neon green, fixed rather than value-coloured: while
            // the two strips are being compared it has to be obvious at a
            // glance which is which, and green reads as distinct from the
            // cyan/amber/red the MCS blocks cycle through.
            canvas_->stacked_bar(bar_x, current_ry, 0.85f * s, h_thin, 0.0f, true,
                             quality, neon_r * dim, neon_g * dim, neon_b * dim);
        } else {
            canvas_->stacked_bar(bar_x, current_ry + h_thin + h_gap, 0.85f * s, h_main, 0.0f, true,
                             1.0f, 1.0f * dim, 0.75f * dim, 0.20f * dim);
        }
        }
        current_ry += total_gauge_h + 0.03f * s;

        // Row 1b: LINK HEALTH - the numeric form of the green strip in the
        // gauge above, not a second opinion. Both come from `quality`.
        //
        // The +/-dB beside it is margin above the downshift threshold for the
        // rung the radio is CURRENTLY on, so it reads as "this much before the
        // bitrate steps down". It used to come from MCS_SNR_FLOOR, a table
        // estimated from walk-test downshifts, which stock's own captured
        // thresholds put roughly 8 dB too high at MCS 12 - the readout was
        // pessimistic enough to show danger while the radio was comfortable.
        if (legacy_panels && connected) {
            float margin_db = (rung >= 0) ? link_copy.snr - kMcsDownDb[rung] : 0.0f;
            float health    = quality * 100.0f;
            auto hc = get_snr_color(health / 100.0f);
            if (hud_verbose) {
                sprintf(buf, "LINK %d%%  %+.1f dB", (int)(health + 0.5f), margin_db);
            } else {
                // Two rows at MEDIUM, so power rides here rather than being
                // dropped with the SNR row: health, headroom and what the
                // radio is spending to hold it, on one line.
                char pw[24]; pwr_text(pw, sizeof(pw));
                sprintf(buf, "LINK %d%%  %+.1f dB  %s", (int)(health + 0.5f), margin_db, pw);
            }
            canvas_->text(buf, outer_x, current_ry, 0.07f * s, true,
                      hc[0] * dim, hc[1] * dim, hc[2] * dim);
            // "STANDBY | " sits on the same line but keeps its own amber, so
            // it reads as a mode flag rather than part of the link health. Only
            // drawn when the VTX is actually in standby (vtx_low_power, from
            // cmd 0x05 bytes 29-30) - NOT air_standby, the TLV 0x12 value, which
            // only echoes the Auto Standby setting. The line is right-aligned,
            // so it is placed by measuring the LINK text.
            if (vtx_low_power) {
                float link_w = canvas_->text_width(buf, 0.07f * s);
                canvas_->text("STANDBY | ", outer_x - link_w, current_ry, 0.07f * s, true,
                          1.0f, 0.75f, 0.20f);
            }
            current_ry += rs;
        } else if (legacy_panels) {
            // Not connected: same line, same shape, amber "IDLE | " flag. The
            // row used to vanish entirely, leaving a gap above the gauge.
            sprintf(buf, "LINK 0%%");
            canvas_->text(buf, outer_x, current_ry, 0.07f * s, true, dim, dim, dim);
            float link_w = canvas_->text_width(buf, 0.07f * s);
            // Same amber as the bar below it, dim included - without the * dim
            // the label read noticeably brighter than the gauge it labels.
            canvas_->text("IDLE | ", outer_x - link_w, current_ry, 0.07f * s, true,
                      1.0f * dim, 0.75f * dim, 0.20f * dim);
            current_ry += rs;
        }

        // Row 2: SNR + TX power
        // The radio answers in dBm; show mW, which is what the menu speaks and
        // what a pilot thinks in. Under AUTO this tracks the level the radio
        // has actually settled on, not the ceiling that was requested.
        //
        // "?" suffix when the air unit reports standby: the value the SDK
        // returns is the configured/requested TX power, not necessarily the
        // actual output. fpv_sky_standby_mode_thread in the air firmware
        // gates the sensor-off + PA-bypass transition behind an internal
        // check (0x6abf0, precise condition not identified), so the air can
        // ACK "standby=ON" while still transmitting at full power - in which
        // case 500mW may be honest, but paired with the STANDBY flag it
        // reads as if it dropped. The "?" makes clear we do not know.
        if (legacy_panels && hud_verbose) {
            char pw[24]; pwr_text(pw, sizeof(pw));
            sprintf(buf, "SNR: %.1f dB  PWR: %s", link_copy.snr, pw);
            canvas_->text(buf, outer_x, current_ry, 0.07f * s, true, dim, dim, dim);
            current_ry += rs;
        }

        // Row 5: antenna gains + LDPC success rate. fec_error is already the
        // pre-FEC codeword error RATIO (baseband reports it x10000, scaled on
        // receive) — success is simply 1 - ratio. The old formula divided one
        // ratio by the other and pinned the display to 100 or 0.
        if (legacy_panels && hud_verbose) {
            float ldpc_ok = std::max(0.0f, std::min(1.0f, 1.0f - link_copy.fec_error)) * 100.0f;
            sprintf(buf, "Gain: %d/%d  LDPC: %.2f%%",
                    link_copy.gains_cur, link_copy.gains_max, ldpc_ok);
            canvas_->text(buf, outer_x, current_ry, 0.07f * s, true, dim, dim, dim);
            current_ry += rs;
        }


        // Row 3: RX MCS + RX data rate. Kept at MEDIUM alongside SNR/PWR:
        // between them they say how hard the radio is working and what it is
        // getting for it, which is the pair worth watching in flight. Gain,
        // LDPC, frequency and bandwidth are diagnostics and stay FULL-only.
        if (legacy_panels) {
            sprintf(buf, "RX MCS%d  %.2f Mbps", ar_mcs_label(link_copy.rx_mcs_val),
                    link_copy.rx_data_rate_kbps / 1000.0f);
            canvas_->text(buf, outer_x, current_ry, 0.07f * s, true, dim, dim, dim);
            current_ry += rs;
        }

        // (Removed) Row 3b used to show the kestrel-air adaptation telemetry
        // ("AIR T0 0.0Mbps 0fps WLd0 0ms"). The AR8030 link carries no such
        // feed, so it only ever rendered zeros.

        // Row 4: TX/RX frequency + RF bandwidth
        // tx_freq only. rx_status reports 2100 MHz on this radio while the link
        // is plainly on 5839, so showing both just raised the question.
        // The gear the radio reports, as MHz. Stock's own fpv_bb_set_bw
        // (0x9fee0) maps 5->2, 10->3, 20->4, 40->5, which matches our
        // bb_bandwidth_e, so the enum and the reading are both sound. Do not
        // put throughput here - the RX MCS row below already carries it.
        if (legacy_panels && hud_verbose) {
            if (link_copy.tx_freq > 0)
                sprintf(buf, "%d MHz  BW: %s MHz",
                        link_copy.tx_freq, gnd::bw_step_label(link_copy.bw_step));
            else
                sprintf(buf, "BW: %s MHz", gnd::bw_step_label(link_copy.bw_step));
            canvas_->text(buf, outer_x, current_ry, 0.07f * s, true, dim, dim, dim);
            current_ry += rs;
        }

        // Distance and temperature used to sit here. They moved to the top
        // status row, where they stay visible at every detail level.



    }
}

// The link's parts and the air unit's as the HUD takes them (hud/parts.hpp),
// the camera's settings as the air unit reports them, the FC's bytes, and
// the link's settings as the menu shows them.
#include "hud/osd_detail.hpp"


void OSD::apply_camera_config(int ev_x10, int sat, int contrast, int sharp,
                              int scene, int awb, int angle,
                              int dnr3d, int focus) {
    for (int i = 0; i < kEvCount; i++)      if (kEvSteps[i]    == ev_x10)  menu_cam_ev      = i;
    for (int i = 0; i < kWbCount; i++)      if (wb_kelvin(i)   == awb)     menu_cam_wb      = i;
    menu_cam_sat      = std::min(kCamSatMax,      std::max(0, sat));
    menu_cam_contrast = std::min(kCamContrastMax, std::max(0, contrast));
    menu_cam_sharp    = std::min(kCamSharpMax,    std::max(0, sharp));
    for (int i = 0; i < kSceneCount; i++) if (kSceneVals[i] == scene) menu_cam_scene = i;
    menu_cam_flip = angle ? 1 : 0;
    for (int i = 0; i < kDnrCount; i++)   if (kDnrVals[i]   == dnr3d) menu_cam_3dnr  = i;
    if (dnr3d == 1) menu_cam_3dnr = 4;   // the old Off: the air runs the bin's, as Auto
    menu_cam_focus = focus ? 1 : 0;
}

void OSD::apply_camera_dnr(int dnr3d) {
    for (int i = 0; i < kDnrCount; i++) if (kDnrVals[i] == dnr3d) menu_cam_3dnr = i;
    if (dnr3d == 1) menu_cam_3dnr = 4;   // the bin's own: Auto
}

void OSD::update_menu_from_tx(int cmd, int val) {
    pthread_mutex_lock(&osd_mutex);
    // The legacy kestrel-air menu rows (bitrate, slice, exposure, ...) and the
    // no-capabilities RF list are gone; their acks are simply ignored here.
    if (cmd == 0x01) menu_video_mode = val;
    else if (cmd == 0x07) { osd_vars.sky_exposure_us = val; sky_exposure_us_ = (uint32_t)val; }
    else if (cmd == 0x08) { osd_vars.sky_framerate = val; sky_framerate_ = (uint32_t)val; }
    else if (cmd == 0x17) {
        // AIR-owned channel MODE (1=auto hop, 0=force). Arrives before 0x11.
        // (This used to skip a goggle-side "pin", read from a pinned_chan
        // setting nothing has written since pinning was removed.)
        air_chan_mode = val;
        if (val == 1) {
            if (!rf_chan_asserted) {
                rf_chan_asserted = true;
                unknown_command(0x107, 1);                 // re-assert AUTO on the AP
            }
        }
    }
    else if (cmd == 0x11) {
        // AIR-owned forced channel: re-assert on our AP radio once per link
        // session (channel is AP-arbitrated and resets to config on re-pair).
        if (air_chan_mode != 1 && !rf_chan_asserted) {
            rf_chan_asserted = true;
            printf("[rf] re-asserting AIR-owned channel %d on the AP\n", val);
            unknown_command(0x107, 0);
            unknown_command(0x101, val);
        }
    }
    else if (cmd == 0x14) {
        // AIR-owned RF bandwidth. The AP arbitrates bandwidth and resets it to
        // config on every re-pair, so re-assert the persisted value ONCE per
        // link session (rf_bw_applied is re-armed on link drop).
        if (val != rf_bw_applied) {
            rf_bw_applied = val;
            printf("[rf] re-asserting AIR-owned bandwidth enum %d on the AP\n", val);
            unknown_command(0x104, val);
        }
    }
    
    menu_synced = true;
    render_requested = true;
    hud_wake_.poke();
    pthread_mutex_unlock(&osd_mutex);
}


void OSD::drain_msp() {
    air::FcChunk c;
    while (parts_.fc.take(c)) {
        // The FC's screen starts over here (a new link, another aircraft):
        // what came before was parsed first, so none of it lands on the
        // clean grid.
        if (c.screen_reset) msp_osd.reset_screen();
        if (c.n) msp_osd.parse_bytes(c.b, c.n);
    }
}

// ---- the link -------------------------------------------------------------

void OSD::unknown_command(int code, int value) { printf("Unknown command 0x%x = %d\n", code, value); }

hud::LinkView OSD::link_view() const {
    return hud::link_view(parts_.radio.state(), parts_.air_radio.state(), parts_.encoder.state(), asked_);
}


void OSD::sync_radio() {
    // The renderer's latency samples too: this runs at every wake of the HUD's
    // loop, whatever it then draws (the gallery returns before the HUD's frame).
    drain_latency_frames();

    // Each version before its state: a newer state is never taken as seen.
    radio_synced_ = parts_.radio.version() + parts_.air_radio.version() + parts_.encoder.version();
    air_synced_ = parts_.air_unit.version() + parts_.camera.version();
    const gnd::RadioState rs = parts_.radio.state();
    osd_vars.radio_link = rs.link;
    link_distance_raw = rs.dist_raw;
    dist_m_ = rs.dist_m;
    rf_caps = rs.rf_caps;
    gnd_live_dbm_.store(rs.live_dbm, std::memory_order_relaxed);
    // Link dropped: re-arm the re-asserts so the next settings broadcast
    // (requested on reconnect) re-applies the AIR-owned values.
    if (rs.link.state != 2) { rf_bw_applied = -1; rf_chan_asserted = false; }

    air_radio_ = parts_.air_radio.state();
    air_live_dbm_.store(air_radio_.live_dbm, std::memory_order_relaxed);
    air_unit_ = parts_.air_unit.state();
    const air::UnitState& as = air_unit_;
    air_feat_ = as.features;
    air_stock_ = as.stock;
    air_max_bw_ = as.max_bw;
    vtx_low_power = as.low_power;
    vtx_temp_valid = as.vtx_temp_valid;
    vtx_cpu_c = as.vtx_cpu_c;
    handshake_sent_us_ = as.handshake_sent_us;
    follow_air(parts_.camera.state());

    const gnd::TimingState vt = parts_.timing.state();
    air_delay_ms = vt.air_delay_ms;
    air_delay_valid = vt.air_delay_valid;
    rsm_latency_ms = vt.rsm_ms;

    float mbps;
    while (parts_.radio.take_rate(mbps)) {
        osd_vars.video_bandwidth = mbps;
        osd_vars.bitrate_history.push_back(mbps);
        if (osd_vars.bitrate_history.size() > 50) osd_vars.bitrate_history.erase(osd_vars.bitrate_history.begin());
        log_csv_row();
    }
    gnd::ScanSample sc;
    while (parts_.radio.take_scan(sc)) apply_chan_scan(sc);
    air::Resend r;
    while (parts_.air_radio.take_resend(r)) {
        resend_hist_.push_back(r);
        if (resend_hist_.size() > 16384) resend_hist_.pop_front();
    }
    const uint64_t drops = parts_.radio.dropped() + parts_.air_radio.resends_dropped();
    if (drops != radio_drops_seen_) {
        printf("[OSD] the radio's rates, scans or resends came faster than the HUD took them: %llu dropped\n",
               (unsigned long long)drops);
        radio_drops_seen_ = drops;
    }
}

// The air unit's settings as it reports them, each report once: the menu
// follows its own values.
void OSD::follow_air(const air::CameraState& c) {
    air::CameraState& seen = camera_seen_;
    // The mode the air unit runs, or is about to be asked for: the row shows
    // it rather than the built-in default.
    if (c.mode_n != seen.mode_n && c.video_mode >= 0 && c.video_mode < (int)video_mode_names.size())
        menu_video_mode = c.video_mode;
    if (c.camera_n != seen.camera_n)
        apply_camera_config(c.ev_x10, c.sat, c.contrast, c.sharp, c.scene, c.awb, c.angle, c.dnr3d, c.focus);
    if (c.dnr_n != seen.dnr_n) apply_camera_dnr(c.dnr3d);
    if (c.angle_n != seen.angle_n) menu_cam_flip = c.angle ? 1 : 0;
    seen = c;
    if (air_unit_.standby_n != standby_seen_n_) {
        // The row in step with the air's own report - but not while the menu
        // is open, so a force the user is dialling in is not clobbered.
        air_standby = air_unit_.standby;
        if (!menu_open) menu_ar_standby = air_unit_.standby;
    }
    standby_seen_n_ = air_unit_.standby_n;
    // Its power levels known: the TX Power row holds the power chosen, shown
    // (and sent on Enter) fitted to this air unit - not before it has said
    // what it is (a Lite+ would be offered a Lite's).
    if (air_radio_.levels_n != levels_seen_n_ && !menu_open) menu_ar_power = link_view().tx_power_want_mw;
    levels_seen_n_ = air_radio_.levels_n;
}

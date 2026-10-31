// The AR8030 link and the air unit: the handshake and its config frame, the
// air unit's status, version, info and announcements, its camera's settings
// and the settings sent to it.
#include "radio/ar8030_link_detail.hpp"

using namespace radio;

void Ar8030Link::publish_air_info() {
    AirInfo a;
    {
        std::lock_guard<bus::PiMutex> g(air_info_mtx_);
        a = air_info_;
    }
    const std::vector<std::string> caps = cap_names(a);
    a.caps_n = 0;
    for (const std::string& c : caps) {
        if (a.caps_n == AirInfo::kMaxCaps) break;
        snprintf(a.caps[a.caps_n++], sizeof(a.caps[0]), "%s", c.c_str());
    }
    model_.air_info(a);
}

const Ar8030Link::AirCap Ar8030Link::kAirCaps[10] = {
    { kAirFeatLatInfo,      "air-timing",    "Air Timing" },
    { kAirFeatApClock,      "radio-clock",   "Radio Clock" },
    { kAirSaidFrameClock,   "frame-clock",   "Frame Clock" },
    { kAirSaidSensorTiming, "sensor-timing", "Sensor Timing" },
    { kAirFeatIntraRefresh, "intra-refresh", "Intra Refresh" },
    { kAirFeatCamImu,       "cam-imu",       "Camera IMU" },
    { kAirFeatFcImu,        "fc-imu",        "FC IMU" },
    { kAirFeatMaxBw,        "bandwidth-cap", "Bandwidth Cap" },
    { kAirFeat864p120,      "864p120",       "864p120" },
    { kAirFeat864p144,      "864p144",       "864p144" },
};

std::vector<std::string> Ar8030Link::cap_names(const AirInfo &a) {
    std::vector<std::string> out;
    const uint16_t have = (uint16_t)(a.feat | a.said);
    for (const auto &c : kAirCaps) if (have & c.bit) out.push_back(c.name);
    return out;
}

void Ar8030Link::clear_air_info() {
    {
        std::lock_guard<bus::PiMutex> g(air_info_mtx_);
        air_info_ = AirInfo();
        air_info_feat_ = 0;
        air_info_seen_ = false;
    }
    publish_air_info();
}

void Ar8030Link::check_air_modes() {
    // Not before it has said what it is (its info can come first).
    if (!air_ver_seen_) return;
    const uint16_t have = air_kestrel_ ? (uint16_t)(air_feat_ | air_info_feat_) : 0;
    const uint16_t known = air_info_seen_ ? 0xffff : 0x00ff;
    for (int i = 0; i < sky::kFpvModeCount; i++) {
        const sky::VideoMode &m = sky::kFpvModes[i];
        if (m.w == sky_cfg.ch0_w && m.h == sky_cfg.ch0_h && m.fps == sky_cfg.ch0_fps && (m.need & known & ~have)) {
            const int d = sky::default_fpv_mode_index();
            printf("ar8030: the air unit has no %s: asking for %s\n", m.name, sky::kFpvModes[d].name);
            model_.update_camera([&](air::CameraState& c) { c.video_mode = d; c.mode_n++; });
            request_mode_index(d);
            break;
        }
    }
}

void Ar8030Link::load_sky_config() {
    // Seed from the stock capture so every field we do not persist keeps the
    // value stock boots with, then let stored settings override.
    const uint8_t *f = ar_hs_5;
    unsigned lf = f[4] | (f[5] << 8);
    sky::read_config_payload(f + 7, (lf >> 4) - 1, &sky_cfg);
    if (sky_cfg_key.empty()) return;

    Settings &st = settings_;
    sky_cfg.ch0_w       = (uint16_t)st.get(radio::setting::air_camera(sky_cfg_key, "ch0_w", sky_cfg.ch0_w));
    sky_cfg.ch0_h       = (uint16_t)st.get(radio::setting::air_camera(sky_cfg_key, "ch0_h", sky_cfg.ch0_h));
    sky_cfg.ch0_fps     = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "ch0_fps", sky_cfg.ch0_fps));
    sky_cfg.ev_x10      = (int8_t)  st.get(radio::setting::air_camera(sky_cfg_key, "ev", sky_cfg.ev_x10));
    sky_cfg.scenes      = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "scene", sky_cfg.scenes));
    sky_cfg.saturation  = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "sat", sky_cfg.saturation));
    sky_cfg.sharpness   = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "sharp", sky_cfg.sharpness));
    sky_cfg.contrast    = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "contrast", sky_cfg.contrast));
    sky_cfg.awb_cct     = (uint16_t)st.get(radio::setting::air_camera(sky_cfg_key, "awb", sky_cfg.awb_cct));
    sky_cfg.anti_flicker= (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "flicker", sky_cfg.anti_flicker));
    sky_cfg.angle       = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "flip", sky_cfg.angle));
    sky_cfg.dnr_3d      = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "dnr3d", sky_cfg.dnr_3d));
    sky_cfg.focus_en    = (uint8_t) st.get(radio::setting::air_camera(sky_cfg_key, "focus", sky_cfg.focus_en));
    printf("ar8030: [%s] restored camera cfg: ch0=%ux%u@%u ev=%d sat=%u sharp=%u "
           "contrast=%u scene=%u awb=%u flicker=%u angle=%u focus=%u\n",
           sky_cfg_key.c_str(), sky_cfg.ch0_w, sky_cfg.ch0_h, sky_cfg.ch0_fps,
           sky_cfg.ev_x10, sky_cfg.saturation, sky_cfg.sharpness, sky_cfg.contrast,
           sky_cfg.scenes, sky_cfg.awb_cct, sky_cfg.anti_flicker, sky_cfg.angle,
           sky_cfg.focus_en);
    // The video mode is this air unit's too: the one the handshake asks for,
    // so nothing asks for another after it (a mode picked for the last unit
    // switched this one's encoder twice at every link-up).
    if (mode_chn == 0 && sky_cfg.ch0_w && sky_cfg.ch0_h && sky_cfg.ch0_fps) {
        mode_w = sky_cfg.ch0_w; mode_h = sky_cfg.ch0_h; mode_fps = sky_cfg.ch0_fps;
        for (int i = 0; i < sky::kFpvModeCount; i++)
            if (sky::kFpvModes[i].w == mode_w && sky::kFpvModes[i].h == mode_h && sky::kFpvModes[i].fps == mode_fps)
                model_.update_camera([&](air::CameraState& c) { c.video_mode = i; c.mode_n++; });
    }

    model_.update_camera([&](air::CameraState& a) {
        a.ev_x10 = sky_cfg.ev_x10; a.sat = sky_cfg.saturation; a.contrast = sky_cfg.contrast;
        a.sharp = sky_cfg.sharpness; a.scene = sky_cfg.scenes; a.awb = sky_cfg.awb_cct;
        a.angle = sky_cfg.angle; a.dnr3d = sky_cfg.dnr_3d; a.focus = sky_cfg.focus_en;
        a.camera_n++;
    });

    // Point the menu row at the mode we are actually about to push. main()
    // publishes the list before the link exists, so without this the row shows
    // the built-in default no matter what the air unit is running.
    {
        for (int i = 0; i < sky::kFpvModeCount; i++) {
            if (sky::kFpvModes[i].w == sky_cfg.ch0_w &&
                sky::kFpvModes[i].h == sky_cfg.ch0_h &&
                sky::kFpvModes[i].fps == sky_cfg.ch0_fps) {
                model_.update_camera([&](air::CameraState& c) { c.video_mode = i; c.mode_n++; });
                break;
            }
        }
    }
}

void Ar8030Link::save_sky_config() {
    if (sky_cfg_key.empty()) return;
    Settings &st = settings_;
    st.set(radio::setting::air_camera(sky_cfg_key, "ch0_w"),   (int)sky_cfg.ch0_w);
    st.set(radio::setting::air_camera(sky_cfg_key, "ch0_h"),   (int)sky_cfg.ch0_h);
    st.set(radio::setting::air_camera(sky_cfg_key, "ch0_fps"), (int)sky_cfg.ch0_fps);
    st.set(radio::setting::air_camera(sky_cfg_key, "ev"),      (int)sky_cfg.ev_x10);
    st.set(radio::setting::air_camera(sky_cfg_key, "scene"),   (int)sky_cfg.scenes);
    st.set(radio::setting::air_camera(sky_cfg_key, "sat"),     (int)sky_cfg.saturation);
    st.set(radio::setting::air_camera(sky_cfg_key, "sharp"),   (int)sky_cfg.sharpness);
    st.set(radio::setting::air_camera(sky_cfg_key, "contrast"),(int)sky_cfg.contrast);
    st.set(radio::setting::air_camera(sky_cfg_key, "awb"),     (int)sky_cfg.awb_cct);
    st.set(radio::setting::air_camera(sky_cfg_key, "flicker"), (int)sky_cfg.anti_flicker);
    st.set(radio::setting::air_camera(sky_cfg_key, "flip"),    (int)sky_cfg.angle);     // read back as "flip" (load_sky_config)
    st.set(radio::setting::air_camera(sky_cfg_key, "dnr3d"),   (int)sky_cfg.dnr_3d);
    st.set(radio::setting::air_camera(sky_cfg_key, "focus"),   (int)sky_cfg.focus_en);
}

// The radio side of the same: the TX power this air unit last ran with. Until
// it has its own, the goggle-wide tx_power_mw main() loaded stands in. The
// channel is not kept - the air unit decides it, and the goggle searches.
void Ar8030Link::load_sky_link() {
    if (sky_cfg_key.empty()) return;
    tx_power_want_mw = settings_.get(radio::setting::air_tx_power_mw(sky_cfg_key, tx_power_want_mw));
    tx_power_mw  = ar_pwr_fit(air_prj.load(), tx_power_want_mw);
    tx_power_dbm = kArPwrLevels[ar_pwr_index(tx_power_mw)].dbm;
    printf("ar8030: [%s] restored radio cfg: power=%s (chosen %s)\n", sky_cfg_key.c_str(),
           kArPwrLevels[ar_pwr_index(tx_power_mw)].label, kArPwrLevels[ar_pwr_index(tx_power_want_mw)].label);
}

// The power chosen, not the one a smaller air unit is held to: a Lite's fit
// saved here would cap the next Lite+ (the units can share one key - one
// pairing - and did, 2026-10-12).
void Ar8030Link::save_sky_link() {
    if (sky_cfg_key.empty()) return;
    settings_.set(radio::setting::air_tx_power_mw(sky_cfg_key, 0), tx_power_want_mw);
}

// Rebuild one captured SET_CONFIG frame around our live settings. The stock
// body is the template, so the bytes we have not identified keep stock values;
// only the known fields and the timestamp change.
// Note for anyone re-sending a config frame outside the handshake: give it a
// fresh sequence number. Replaying the captured seq makes the air treat the
// frame as a duplicate and drop it silently - it is not acked and has no
// effect. Proved while chasing 3D DNR (references/sky-commands.md).
std::vector<uint8_t> Ar8030Link::build_config_frame(const uint8_t *tmpl, size_t len) {
    unsigned lf = tmpl[4] | (tmpl[5] << 8);
    size_t bodylen = (lf >> 4) - 1;
    std::vector<uint8_t> body(tmpl + 7, tmpl + 7 + bodylen);
    sky::patch_config_payload(body.data(), body.size(), sky_cfg);
    // Body offset 45 (u16 LE) is the TX power, mW, which the air unit applies
    // after the power frame. Left as captured it was stock's 500mW Auto (501):
    // every handshake - each link-up, each 2.5 s of quiet video - undid the
    // chosen power, and asked an Ascent Lite (100 mW) for 26 dBm.
    const int mw = kArPwrLevels[ar_pwr_index(tx_power_mw)].mw;
    if (body.size() >= 47) { body[45] = (uint8_t)(mw & 0xFF); body[46] = (uint8_t)((mw >> 8) & 0xFF); }
    sky_proto.set_seq(tmpl[3]);                       // replaying stock's handshake
    return sky_proto.build(tmpl[6], body.data(), body.size());
}

// The air unit stays silent until the ground unit sends these frames on the
// port-2 (telemetry) socket - captured from a clean boot of the stock app.
// Opening the video socket alone yields at most a short burst of whatever was
// already buffered.
void Ar8030Link::send_air_handshake() {
    // The acquiring screen distinguishes "link up, nothing asked for yet"
    // from "asked, nothing came back", which is the difference between
    // waiting and being ignored.
    model_.update_unit([](air::UnitState &a) { a.handshake_sent_us = now_us(); });
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;

    if (ctrl_sockfd < 0) {
        bb_sock_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.tx_buf_size = 0x0800;
        opt.rx_buf_size = 0x0800;          // stock uses 2 KB here, 56 KB for video
        ctrl_sockfd = bb_socket_open(dev, (bb_slot_e)slot, 2,
                                     BB_SOCK_FLAG_RX | BB_SOCK_FLAG_TX, &opt);
        printf("ar8030: control socket (port 2) fd=%d\n", ctrl_sockfd);
        if (ctrl_sockfd < 0) return;
    }

    identify_peer();
    load_sky_config();
    for (size_t i = 0; i < ar_handshake_n; i++) {
        const uint8_t *data = ar_handshake[i].data;
        size_t         len  = ar_handshake[i].len;
        // The two SET_CONFIG frames carry the camera settings the VTX will run
        // with, so build them from our stored config instead of replaying the
        // capture - otherwise a menu change is undone at the next handshake.
        std::vector<uint8_t> rebuilt;
        const char *from = "";
        if (data[6] == sky::CMD_SET_CONFIG) {
            rebuilt = build_config_frame(data, len);
            data = rebuilt.data(); len = rebuilt.size();
            from = "   (video config, from stored settings)";
        } else if (data[6] == sky::CMD_SET_BB_PWR) {
            // Stock's two power frames carry its own 500mW Auto. This air
            // unit's power instead, so a link-up never sets it to anything
            // else first. Same u16 LE payload as request_setting(CAM_PWR).
            const int mw = kArPwrLevels[ar_pwr_index(tx_power_mw)].mw;
            const uint8_t v[2] = { (uint8_t)(mw & 0xFF), (uint8_t)((mw >> 8) & 0xFF) };
            sky_proto.set_seq(data[3]);
            rebuilt = sky_proto.build(data[6], v, sizeof(v));
            data = rebuilt.data(); len = rebuilt.size();
            from = "   (air power, from stored settings)";
        }
        int w = bb_socket_write(ctrl_sockfd, data, (uint32_t)len, 500);
        printf("ar8030: handshake[%zu] seq=0x%02X len=%zu -> %d%s\n",
               i, data[3], len, w, from);
        usleep(250000);  // spread the PA burst: back-to-back TX appears to brown the board out
        if (*should_stop) { printf("ar8030: handshake aborted (stopping)\n"); return; }
    }

    // The replayed frames end at seq 0x09; anything we synthesise from here on
    // has to carry on from there or the air unit drops it as out of order.
    sky_proto.set_seq(ar_handshake[ar_handshake_n - 1].data[3] + 1);
    if (probe_rf) probe_rf_ioctls();
    if (probe_modes) probe_video_modes();
    // The resolution is the handshake's (this air unit's stored mode): no
    // mode switch after it - that is for a different mode picked in the menu.
}

// Read frames off the port-2 socket until one acknowledges want_cmd. The air
// unit interleaves unsolicited telemetry here, so we skip anything else.
bool Ar8030Link::read_sky_ack(uint8_t want_cmd, int timeout_ms) {
    if (ctrl_sockfd < 0) return false;
    uint8_t buf[512];
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (*should_stop) return false;
        int n = bb_socket_read(ctrl_sockfd, buf, sizeof(buf), 50);
        if (n <= 0) continue;   // -1 is a plain timeout, not an error
        on_ctrl_bytes(buf, n);     // the air's reports, MSP, an awaited ack: not lost here
        // Frames can be batched in one read; walk every FE A5 boundary.
        for (int i = 0; i + 12 <= n; i++) {
            if (buf[i] != 0xFE || buf[i + 1] != 0xA5) continue;
            unsigned len = ((buf[i + 4] | (buf[i + 5] << 8)) >> 4) + 11;
            if (i + (int)len > n) continue;
            uint8_t cmd = 0, status = 0; bool is_ack = false;
            if (!sky::Proto::parse(buf + i, len, &cmd, &is_ack, &status)) continue;
            if (is_ack && cmd == want_cmd) {
                printf("ar8030: sky ack cmd=0x%02X status=%u (%s)\n",
                       cmd, status, status == 0 ? "accepted" : "REJECTED");
                return status == 0;
            }
        }
    }
    printf("ar8030: no ack for cmd 0x%02X within %d ms\n", want_cmd, timeout_ms);
    return false;
}

// What the control socket brought in, wherever it was read (the main loop's
// drain, or a wait for a mode change's ack): the air's reports, the flight
// controller's MSP riding it, and the ack a camera setting is waiting for.
void Ar8030Link::on_ctrl_bytes(const uint8_t *buf, int n) {
    ctrl_bytes += (unsigned long long)n;
    last_ctrl_ms = now_ms();            // the air unit is talking to us
    scan_air_status(buf, n);
    if (sky_ack_.field >= 0) {
        // Frames can be batched in one read; walk every FE A5 boundary.
        for (int i = 0; i + 12 <= n; i++) {
            if (buf[i] != 0xFE || buf[i + 1] != 0xA5) continue;
            unsigned len = ((buf[i + 4] | (buf[i + 5] << 8)) >> 4) + 11;
            if (i + (int)len > n) continue;
            uint8_t cmd = 0, status = 0; bool is_ack = false;
            if (!sky::Proto::parse(buf + i, len, &cmd, &is_ack, &status)) continue;
            if (is_ack && cmd == sky_ack_.cmd) {
                sky_ack_done(status);
                break;
            }
        }
    }
    // --debug-msp-bb-port 2 means "the FC stream is inside the telemetry we
    // already drain" - tee it rather than opening a second socket on a
    // port this client already owns.
    if (msp_bb_port == 2 && fc_out_.watched()) {
        msp_bytes += (unsigned long long)n;
        fc_out_.bytes(buf, (size_t)n);
    }
}

// The awaited camera setting acknowledged: accepted, it goes into the stored
// config, which every handshake sends the air unit again.
void Ar8030Link::sky_ack_done(int status) {
    printf("ar8030: sky ack cmd=0x%02X status=%d (%s)\n",
           sky_ack_.cmd, status, status == 0 ? "accepted" : "REJECTED");
    if (status == 0) {
        const int v = sky_ack_.value;
        switch (sky_ack_.field) {
            case CAM_3DNR:      sky_cfg.dnr_3d       = (uint8_t)v;  break;
            case CAM_EV:        sky_cfg.ev_x10       = (int8_t)v;   break;
            case CAM_SAT:       sky_cfg.saturation   = (uint8_t)v;  break;
            case CAM_CONTRAST:  sky_cfg.contrast     = (uint8_t)v;  break;
            case CAM_SHARPNESS: sky_cfg.sharpness    = (uint8_t)v;  break;
            case CAM_SCENE:     sky_cfg.scenes       = (uint8_t)v;  break;
            case CAM_AWB:       sky_cfg.awb_cct      = (uint16_t)v; break;
            case CAM_ANGLE:     sky_cfg.angle        = (uint8_t)v;  break;
            case CAM_FOCUS:     sky_cfg.focus_en     = (uint8_t)v;  break;
            // CAM_STANDBY deliberately absent: its offset inside the
            // SET_CONFIG body is not known, so writing it there would
            // corrupt a field we do not understand.
            default: break;
        }
        save_sky_config();
    }
    sky_ack_ = SkyAckWait();
}

void Ar8030Link::scan_air_status(const uint8_t *buf, int n) {
    for (int i = 0; i + 8 <= n; i++) {
        if (buf[i] != 0xFE || buf[i + 1] != 0xA5) continue;
        unsigned len = ((buf[i + 4] | (buf[i + 5] << 8)) >> 4) + 11;
        if (len < 12 || i + (int)len > n) continue;

        // First sighting only for each inbound sky cmd. The per-change dump
        // that lived here through the section 40 investigation identified
        // cmd 0x05 as the VTX telemetry frame (see the decoder below), so
        // the change spam is no longer needed and the log stays quiet.
        {
            static bool seen_cmd[256] = { false };
            uint8_t cmd_byte = buf[i + 6];
            if (!seen_cmd[cmd_byte]) {
                seen_cmd[cmd_byte] = true;
                const uint8_t *pl = buf + i + 6;
                size_t pl_len = len - 6 - 5;
                printf("ar8030: inbound cmd 0x%02x payload=%zu bytes:", cmd_byte, pl_len);
                for (size_t k = 0; k < pl_len && k < 96; k++) printf(" %02X", pl[k]);
                if (pl_len > 96) printf(" ...");
                printf("  (first sighting - sec 40)\n");
            }
        }

        // cmd 0x05 - VTX periodic telemetry. Identified by:
        //   (a) the air unit's own binary has fpv_cmd_get_rf_temp() which
        //       reads a global at [ctx+0x3fc] and packs it as a °C byte;
        //       fpv_bb_update_rf_board_temp() computes that value from a
        //       thermistor ADC via an interpolated LUT (see section 40b);
        //   (b) empirically, byte 30 fluctuates by ~1-2°C every frame -
        //       characteristic ADC noise on a live sensor - and byte 33
        //       fell in lockstep with TX power dropping from 501 to 200 mW
        //       (bytes 9-10 in the same frame), which is exactly RF PA
        //       cooling behaviour.
        // So byte 30 is the SoC/CPU temperature and byte 33 is the RF
        // board temperature, both single-byte °C. Publish the max as the
        // VTX temperature - whichever is closer to a thermal limit is the
        // number that matters. Log periodically for post-mortems.
        if (buf[i + 6] == 0x04) note_air_version(buf + i + 6, len - 6 - 5);
        if (buf[i + 6] == 0x50) note_air_info(buf + i + 6, len - 6 - 5);
        if (buf[i + 6] == 0x51) note_air_frame(buf + i + 6, len - 6 - 5);

        if (buf[i + 6] == 0x05 && (len - 6 - 5) >= 34) {
            const uint8_t *pl = buf + i + 6;
            // VTX CPU temp = byte 1, VTX RF board temp = byte 33, both °C.
            // Ground-truth confirmed against the stock app's debug OSD, which
            // shows "S TEMP: 50" for the VTX SoC while byte 1 read 0x30=48;
            // the 2°C offset is normal sensor noise, and byte 1 has drifted
            // 42-52°C across many samples in exactly the range stock displays.
            //
            // Two earlier hypotheses got this wrong: (1) byte 30 alone gave
            // a bimodal 59/63 alias and jumped to 148 out of standby; (2)
            // bytes 29-30 as BE u16 / 10 read 83°C in standby but stock
            // clearly shows 50°C for the same state, so that field is not
            // temperature - see below for what it turned out to be instead.
            uint8_t vtx_cpu = pl[1];
            model_.update_unit([&](air::UnitState &a) { a.vtx_cpu_c = (float)vtx_cpu; a.vtx_temp_valid = true; });

            static uint64_t vtx_log_ms = 0;
            uint64_t vtx_now = now_ms();
            if (vtx_now - vtx_log_ms >= 5000) {
                vtx_log_ms = vtx_now;
                printf("ar8030: VTX cpu=%uC (cmd 0x05 b1)\n", vtx_cpu);
            }

            // Bytes 29-30 track whether the VTX is actually radiating at
            // normal power, not standby/low-power - a real, live signal
            // rather than the (confirmed unreliable, see
            // ar8030-power-and-standby-verified.md) TLV_STANDBY_MODE the air
            // separately reports. Confirmed on the bench: TLV 0x12 stayed
            // latched at standby=ON through an entire arm/disarm cycle,
            // while this field cleanly stepped from ~840 (idle) to ~714
            // (armed) in lockstep with VTX temperature climbing 47C->64C -
            // a ~130-count gap with no overlap across ~45 samples each side.
            // kVtxStandbyThresh sits in the middle of that gap; it is a
            // two-sample calibration and may need retuning against more
            // hardware/distance/thermal conditions.
            static constexpr uint16_t kVtxStandbyThresh = 780;
            uint16_t b2930 = ((uint16_t)pl[29] << 8) | pl[30];
            bool low_power = b2930 >= kVtxStandbyThresh;
            model_.low_power(low_power);

            static uint16_t last_b2930 = 0xFFFF;
            static bool     b2930_seen = false;
            if (!b2930_seen || b2930 != last_b2930) {
                b2930_seen  = true;
                last_b2930  = b2930;
                printf("ar8030: VTX bytes[29:30]=%u (0x%04X) cpu=%uC "
                       "-> %s\n",
                       b2930, b2930, vtx_cpu, low_power ? "STANDBY" : "active");
            }
        }

        if (buf[i + 6] != 0x03) { i += len - 1; continue; }


        // payload starts at the cmd byte; drop the 4-byte CRC and trailing BB.
        const uint8_t *pl = buf + i + 6;
        size_t pl_len = len - 6 - 5;
        // The air unit's own config rides in the same frame - "49-byte body +
        // TLV tail", the same shape as the outbound SET_CONFIG (section 32).
        // Logged once, not applied: the field offsets were recovered from the
        // *outbound* serialiser, and feeding a mis-parsed body back into
        // sky_cfg would push corruption to the VTX on the next handshake.
        // Compare it against the "restored camera cfg" line above before
        // trusting it.
        if (pl_len > 1) {
            sky::SkyConfig air;
            if (sky::read_config_payload(pl + 1, pl_len - 1, &air)) {
                // Re-report whenever the camera's own view changes, not just
                // once: this is the only read-back that says whether a setting
                // actually reached it. The ack channel cannot be relied on -
                // see KNOWN_ISSUES.
                if (!air_cfg_logged || air.angle != last_air_angle ||
                    air.ch0_w != last_air_w || air.ch0_h != last_air_h ||
                    air.ch0_fps != last_air_fps || air.dnr_3d != last_air_dnr3d ||
                    air.focus_en != last_air_focus) {
                    air_cfg_logged = true;
                    last_air_angle = air.angle;
                    imu_out_.camera_angle(air.angle ? 180 : 0);   // the picture turns the gyro's axes too
                    last_air_w = air.ch0_w; last_air_h = air.ch0_h;
                    last_air_fps = air.ch0_fps;
                    last_air_dnr3d = air.dnr_3d;
                    last_air_focus = air.focus_en;
                    // The level it runs, which nothing on this side stores: the
                    // menu showed the stock capture's byte instead (Auto, or Off
                    // before) while the air unit ran its own (High by default).
                    // Kept, as the menu's settings are: a handshake restores
                    // the stored config, and the stock capture's byte would win.
                    if (sky_cfg.dnr_3d != air.dnr_3d) {
                        sky_cfg.dnr_3d = air.dnr_3d;
                        save_sky_config();
                    }
                    model_.update_camera([&](air::CameraState& c) { c.dnr3d = air.dnr_3d; c.dnr_n++; });
                    // Its rotation likewise: the menu shows what it runs.
                    model_.update_camera([&](air::CameraState& c) { c.angle = air.angle; c.angle_n++; });
                    printf("ar8030: air unit reports ch0=%ux%u@%u ev=%d sat=%u "
                           "sharp=%u contrast=%u scene=%u awb=%u flicker=%u "
                           "angle=%u dnr3d=%u focus=%u\n",
                           air.ch0_w, air.ch0_h, air.ch0_fps, air.ev_x10,
                           air.saturation, air.sharpness, air.contrast,
                           air.scenes, air.awb_cct, air.anti_flicker, air.angle,
                           air.dnr_3d, air.focus_en);
                }
            }
        }

        // What the air unit is decides which power levels it is offered, as on
        // stock. The power chosen is fitted to them - brought down to the
        // nearest one a Lite is offered, or back up to the chosen one for a
        // Lite+ after a Lite - and sent to it; the choice itself is kept.
        int rf_hw = sky::Proto::find_tlv(pl, pl_len, sky::Proto::TLV_SKY_RF_HWVER);
        if (rf_hw >= 0) {
            bool changed;
            {
                std::lock_guard<bus::PiMutex> g(air_info_mtx_);
                changed = air_info_.rf_hw != rf_hw;
                air_info_.rf_hw = rf_hw;
            }
            if (changed) publish_air_info();
        }
        note_air_prj(sky::Proto::find_tlv(pl, pl_len, sky::Proto::TLV_SKY_PRJ_NAME), "settings");

        int sb = sky::Proto::find_tlv(pl, pl_len, sky::Proto::TLV_STANDBY_MODE);
        if (sb >= 0) {
            bool now = (sb != 0);
            if (!air_status_seen || now != air_standby) {
                printf("ar8030: air unit standby/low-power = %s\n", now ? "ON" : "OFF");
                air_standby = now;
                air_status_seen = true;
                model_.update_unit([&](air::UnitState& u) { u.standby = now; u.standby_n++; });
            }
        }

        i += len - 1;
    }
}

// Wait for an ack of want_cmd, draining video meanwhile.
// Returns the ack status (0 = accepted, >0 = refused), or -1 on timeout.
int Ar8030Link::wait_sky_status(uint8_t want_cmd, int timeout_ms) {
    uint8_t buf[512];
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (*should_stop) return -1;
        drain_video_socket();
        int n = bb_socket_read(ctrl_sockfd, buf, sizeof(buf), 20);
        if (n <= 0) continue;
        on_ctrl_bytes(buf, n);     // the air's reports, MSP, an awaited ack: not lost here
        for (int i = 0; i + 12 <= n; i++) {
            if (buf[i] != 0xFE || buf[i + 1] != 0xA5) continue;
            unsigned len = ((buf[i + 4] | (buf[i + 5] << 8)) >> 4) + 11;
            if (i + (int)len > n) continue;
            uint8_t cmd = 0, status = 0; bool is_ack = false;
            if (!sky::Proto::parse(buf + i, len, &cmd, &is_ack, &status)) continue;
            if (is_ack && cmd == want_cmd) return (int)status;
        }
    }
    return -1;
}

// Drain whatever the menu parked and push it to the camera. Each of these is a
// 4-byte little-endian value; the ids come from the GUI dispatcher in
// ar_ldy_gnd.
// One at a time, as when each waited here for its ack - the video thread
// then stopped reading for up to a second per setting, the radio's ring
// overflowed and the picture broke up until the next keyframe. Now the ack is
// awaited between pictures (on_ctrl_bytes) and the next goes when it is in.
void Ar8030Link::apply_pending_settings() {
    {
        std::lock_guard<std::mutex> lk(pending_mtx);
        settings_q_.insert(settings_q_.end(), pending_settings.begin(), pending_settings.end());
        pending_settings.clear();
    }
    if (ctrl_sockfd < 0) {               // no link: dropped, as they always were
        settings_q_.clear();
        sky_ack_ = SkyAckWait();
        return;
    }
    if (sky_ack_.field >= 0) {
        if (now_ms() < sky_ack_.until_ms) return;
        printf("ar8030: no ack for cmd 0x%02X within %d ms\n", sky_ack_.cmd, kSkyAckMs);
        sky_ack_ = SkyAckWait();
    }
    while (!settings_q_.empty()) {
        const std::pair<int,int> item = settings_q_.front();
        settings_q_.pop_front();
        uint8_t cmd; const char* name;
        switch (item.first) {
            case CAM_EV:        cmd = sky::CMD_SET_EV;           name = "EV";           break;
            case CAM_SAT:       cmd = sky::CMD_SET_SAT;          name = "saturation";   break;
            case CAM_CONTRAST:  cmd = sky::CMD_SET_CONTRAST;     name = "contrast";     break;
            case CAM_SHARPNESS: cmd = sky::CMD_SET_SHARPNESS;    name = "sharpness";    break;
            case CAM_SCENE:     cmd = sky::CMD_SET_SCENES;       name = "scene";        break;
            case CAM_AWB:       cmd = sky::CMD_SET_AWB;          name = "white balance";break;
            case CAM_ANGLE:     cmd = sky::CMD_SET_ANGLE;       name = "rotate";       break;
            case CAM_3DNR:      cmd = sky::CMD_SET_3DNR;       name = "3D DNR";       break;
            case CAM_STANDBY:   cmd = sky::CMD_SET_STANDBY;      name = "standby";      break;
            case CAM_BW:        cmd = sky::CMD_SET_BB_BANDWIDTH; name = "air bandwidth";break;
            case CAM_MAX_KBPS:  cmd = sky::CMD_KA_MAX_BITRATE; name = "video bitrate cap"; break;
            case CAM_MAX_BW:    cmd = sky::CMD_KA_MAX_BW;      name = "video link bandwidth cap MHz"; break;
            case CAM_AIR_LOGS:  cmd = sky::CMD_KA_LOGS;        name = "air flight logs"; break;
            case CAM_PWR:       cmd = sky::CMD_SET_BB_PWR;      name = "air power mW"; break;
            case CAM_FOCUS:     cmd = sky::CMD_SET_CHN_FOCUS;   name = "focus mode";   break;
            default: continue;
        }
        // kestrel-air's own commands: kept for when a kestrel-air announces
        // itself (note_air_announce sends them then), never to the stock app.
        if ((item.first == CAM_MAX_KBPS || item.first == CAM_MAX_BW || item.first == CAM_AIR_LOGS) && !air_kestrel_) {
            printf("ar8030: %s = %d kept for a kestrel-air (this air unit %s)\n", name, item.second,
                   air_ver_seen_ ? "is the stock app" : "has not said what it is yet");
            continue;
        }
        // ar_ldy_gnd 0x89060 builds this one as {cmd 0x23, len 1}: a single
        // byte, unlike most camera settings, which are a u32.
        // Payload width differs per command: ar_ldy_gnd 0x88fd8 writes cmd 0x22
        // as two little-endian bytes (len 2), 0x89060 writes standby as one,
        // and most camera settings are u32.
        std::vector<uint8_t> f;
        if (item.first == CAM_PWR) {
            uint8_t v[2] = { (uint8_t)(item.second & 0xFF),
                             (uint8_t)((item.second >> 8) & 0xFF) };
            f = sky_proto.build(cmd, v, sizeof(v));
        } else if (item.first == CAM_FOCUS) {
            // {u8 chn, u8 enable} - ar_ldy_gnd's own debug line for this
            // command is "GUI_CMD_SET_CHN_FOCUS, chn=%d, en=%d". chn 0 is
            // the FPV stream, the only channel this menu ever controls.
            uint8_t v[2] = { 0, (uint8_t)item.second };
            f = sky_proto.build(cmd, v, sizeof(v));
        } else if (item.first == CAM_STANDBY || item.first == CAM_BW ||
                   item.first == CAM_MAX_BW || item.first == CAM_AIR_LOGS) {
            f = sky_proto.build_u8(cmd, (uint8_t)item.second);
        } else {
            f = sky_proto.build_u32(cmd, (uint32_t)item.second);
        }
        int w = bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 500);
        printf("ar8030: set %s = %d (cmd 0x%02X) -> %d\n", name, item.second, cmd, w);
        if (w > 0) {
            sky_ack_.field = item.first;
            sky_ack_.value = item.second;
            sky_ack_.cmd = cmd;
            sky_ack_.until_ms = now_ms() + kSkyAckMs;
            return;                      // the next once this one's ack is in
        }
    }
}

// --ar8030-mode: ask the air unit to switch the FPV channel to a new
// resolution/frame rate. This is GUI_CMD_SET_CHN_RES in the stock app - the
// same 6-byte SET_CHN_RES command its menu sends when you pick a video mode.
void Ar8030Link::apply_video_mode() {
    if (!mode_w || !mode_h || !mode_fps) return;
    if (ctrl_sockfd < 0) return;

    std::vector<uint8_t> f = sky_proto.set_chn_res((uint8_t)mode_chn,
                                                   (uint16_t)mode_w,
                                                   (uint16_t)mode_h,
                                                   (uint8_t)mode_fps);
    int w = bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 500);
    printf("ar8030: SET_CHN_RES ch%d %dx%d@%d seq=0x%02X -> %d\n",
           mode_chn, mode_w, mode_h, mode_fps, f[3], w);
    if (w > 0 && read_sky_ack(sky::CMD_SET_CHN_RES, 1000) && mode_chn == 0) {
        // Accepted: remember it so the next handshake asks for the same mode.
        sky_cfg.ch0_w   = (uint16_t)mode_w;
        sky_cfg.ch0_h   = (uint16_t)mode_h;
        sky_cfg.ch0_fps = (uint8_t)mode_fps;
        save_sky_config();
    }
}

// cmd 0x04, the air unit's version message. pl[0] is the cmd byte. It is only sent
// in some states of the air app, so a slice header with the tag (below) counts first.
void Ar8030Link::note_air_version(const uint8_t *pl, size_t n) {
    if (n < 8) return;
    const bool ka = pl[1] == 'K' && pl[2] == 'A';
    if (n >= 14) {
        // Bytes 9, 11, 13: the stock firmware's APP_VERSION; 5: the board-ID
        // version (kestrel-air sends them as stock does).
        {
            std::lock_guard<bus::PiMutex> g(air_info_mtx_);
            air_info_.version = true;
            air_info_.kestrel = ka;
            air_info_.stock[0] = pl[9] | (pl[10] << 8);
            air_info_.stock[1] = pl[11] | (pl[12] << 8);
            air_info_.stock[2] = pl[13] | (pl[14] << 8);
            air_info_.hw = pl[5];
            air_info_.chipid = ka ? 0 : (uint32_t)(pl[1] | (pl[2] << 8) | (pl[3] << 16) | ((uint32_t)pl[4] << 24));
        }
        publish_air_info();
    }
    if (!ka && air_hdr_tag_) return;
    note_air_announce(ka, ka ? pl[3] : 0, ka ? pl[4] : 0);
}

// 0x51, kestrel-air's frame (protocol/kestrel_air.h KA_MSG_FRAME): u8
// generation, u32 the frame (us x 256), u32 where its video part ends on the
// radio clock (us x 256), u8 radio frames a picture under the frame lock: the
// frame clock counts our radio's edges from it.
void Ar8030Link::note_air_frame(const uint8_t *pl, size_t n) {
    if (n < 10) return;
    uint32_t tq, pq;
    memcpy(&tq, pl + 2, 4);
    memcpy(&pq, pl + 6, 4);
    if (tq < 1000 * 256 || tq > 20000 * 256 || pq >= tq) return;
    frame_clock_.set_air_frame(tq / 256.0, pq / 256.0, pl[1], n >= 11 ? pl[10] : 0);
    bool changed;
    {
        std::lock_guard<bus::PiMutex> g(air_info_mtx_);   // once a second
        changed = !(air_info_.said & kAirSaidFrameClock);
        air_info_.said |= kAirSaidFrameClock;
    }
    if (changed) publish_air_info();
}

// 0x50, kestrel-air's info message: "key=value" lines (protocol/kestrel_air.h
// there, KA_MSG_INFO). pl[0] is the cmd byte. Logged when it changes.
void Ar8030Link::note_air_info(const uint8_t *pl, size_t n) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::string text((const char *)pl + 1, n > 0 ? n - 1 : 0), line;
    for (size_t a = 0; a < text.size();) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        line = text.substr(a, e - a);
        a = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string v = line.substr(eq + 1);
        for (char &c : v) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '?';
        kv.emplace_back(line.substr(0, eq), v);
    }
    // Its board type (stock's: 482 Lite, 472 Lite+) says what it is too.
    for (const auto &e : kv)
        if (e.first == "board") {
            const long b = strtol(e.second.c_str(), nullptr, 10);
            note_air_prj(b == 472 ? AR_AIR_LITE_PLUS : b == 482 ? AR_AIR_LITE : 0, "info");
        }
    // The modes it says it has beyond its header's bits (864p144): the Video
    // Mode row offers them, and a stored one it lacks goes back to the default.
    {
        uint16_t xf = 0;
        for (const auto &e : kv)
            if (e.first == "864p144" && e.second == "1") xf |= kAirFeat864p144;
        const bool first = !air_info_seen_;
        air_info_seen_ = true;
        if (first || xf != air_info_feat_) {
            air_info_feat_ = xf;
            const uint16_t f = air_kestrel_ ? (uint16_t)(air_feat_ | xf) : 0;
            model_.update_unit([&](air::UnitState &a) { a.features = f; });
            check_air_modes();
        }
    }
    // The sensor's readout and exposure: ours, they change as it runs, so not
    // part of what is logged or shown.
    for (auto it = kv.begin(); it != kv.end();) {
        const long r = strtol(it->second.c_str(), nullptr, 10);
        if (it->first == "readout") air_readout_us_ = r >= 1000 && r <= 40000 ? (uint32_t)r : 0;
        else if (it->first == "exposure") air_exposure_us_ = r > 0 && r <= 200000 ? (uint32_t)r : 0;
        else { ++it; continue; }
        it = kv.erase(it);
    }
    // As the INFO tab shows it: as many lines as fit, cut to their slots.
    AirInfo got;
    for (const auto &e : kv) {
        if (got.kv_n == AirInfo::kMaxKv) break;
        AirInfo::Kv &o = got.kv[got.kv_n++];
        snprintf(o.key, sizeof(o.key), "%s", e.first.c_str());
        snprintf(o.value, sizeof(o.value), "%s", e.second.c_str());
    }
    bool same;
    {
        std::lock_guard<bus::PiMutex> g(air_info_mtx_);
        // For the INFO tab's capabilities: 864p144 as the info says it now; a
        // readout said once in this link stays said (a new mode's comes late).
        air_info_.said = (uint16_t)((air_info_.said & ~kAirFeat864p144) | air_info_feat_ |
                                    (air_readout_us_ ? kAirSaidSensorTiming : 0));
        same = got.kv_n == air_info_.kv_n;
        for (int i = 0; same && i < got.kv_n; i++)
            same = strcmp(got.kv[i].key, air_info_.kv[i].key) == 0 && strcmp(got.kv[i].value, air_info_.kv[i].value) == 0;
        if (!same) {
            air_info_.kv_n = got.kv_n;
            memcpy(air_info_.kv, got.kv, sizeof(got.kv));
        }
    }
    publish_air_info();
    if (same) return;
    printf("ar8030: air unit info:");
    for (const auto &e : kv) printf(" %s=%s", e.first.c_str(), e.second.c_str());
    printf("\n");
}

// What the air unit is (its project number: 4 Lite, 7 Lite+), from its camera
// settings (TLV 0x11, stock's way) or kestrel-air's info (its board type):
// the power levels it is offered, and the chosen power fitted to them.
// kestrel-air sends its settings only after the link-up handshake reaches it,
// which a kestrel-air started after that never had - the goggle then took a
// Lite+ for a Lite and capped it at 100mW AUTO (2026-10-12): the info, every
// 2 s, says it anyway.
void Ar8030Link::note_air_prj(int prj, const char *from) {
    if (prj <= 0 || prj == air_prj.load()) return;
    air_prj.store(prj);
    ar_pwr_offer o = ar_pwr_offered(prj);
    printf("ar8030: air unit is an %s (prj %d, from its %s): power up to %s\n", ar_air_name(prj), prj, from,
           kArPwrLevels[ar_pwr_index(o.mw[o.n - 1])].label);
    int fit = ar_pwr_fit(prj, tx_power_want_mw);
    if (fit != tx_power_mw) {
        printf("ar8030: power for it: %s (chosen %s, was %s)\n", kArPwrLevels[ar_pwr_index(fit)].label,
               kArPwrLevels[ar_pwr_index(tx_power_want_mw)].label, kArPwrLevels[ar_pwr_index(tx_power_mw)].label);
        request_rf(RF_TX_POWER_FIT, fit);
    }
    model_.update_air_radio([&](air::RadioState& r) { r.prj = prj; r.levels_n++; });
}

void Ar8030Link::note_air_announce(bool ka, uint8_t proto, uint8_t feat) {
    {
        std::lock_guard<bus::PiMutex> g(air_info_mtx_);
        air_info_.kestrel = ka;
        air_info_.proto = ka ? proto : 0;
        air_info_.feat = ka ? feat : 0;
    }
    publish_air_info();
    if (air_ver_seen_ && ka == air_kestrel_ && proto == air_proto_ && feat == air_feat_) return;
    air_ver_seen_ = true;
    air_kestrel_ = ka;
    air_proto_ = proto;
    air_feat_ = feat;
    model_.update_unit([&](air::UnitState &a) {
        a.max_bw = ka && (feat & kAirFeatMaxBw);
        a.features = ka ? (uint16_t)(feat | air_info_feat_) : 0;
        a.stock = !ka;
    });
    // The mode pushed at link-up, if this air unit does not have it (864p120
    // kept from before a downgrade, or a stock air app): back to the default.
    check_air_modes();
    lat_enc_.clear(); lat_queue_.clear(); lat_write_.clear();
    lat_depth_max_ = 0;
    if (ka) {
        printf("ar8030: the air unit is kestrel-air, protocol %u, features:%s%s%s%s%s%s%s\n", proto,
               (feat & kAirFeatLatInfo) ? " air-side-times" : "",
               (feat & kAirFeatIntraRefresh) ? " intra-refresh" : "",
               (feat & kAirFeatCamImu) ? " cam-imu" : "",
               (feat & kAirFeatFcImu) ? " fc-imu" : "",
               (feat & kAirFeatApClock) ? " us-radio-clock" : "",
               (feat & kAirFeatMaxBw) ? " max-bandwidth" : "",
               (feat & kAirFeat864p120) ? " 864p120" : "");
        // Its own settings, which it keeps in RAM only: sent once it has said
        // it is kestrel-air, at every link-up (the announcement comes again),
        // whichever of the two units was powered first. The bandwidth cap
        // too when 40: an air unit that kept a 20 must hear it is lifted -
        // but only to one that widens the link at all (the feature).
        if (max_kbps > 0) request_setting(CAM_MAX_KBPS, max_kbps);
        if (feat & kAirFeatMaxBw) request_setting(CAM_MAX_BW, max_bw_mhz.load() == 0 ? 40 : max_bw_mhz.load());
        // Off too: an air unit that kept running since an on is still logging.
        request_setting(CAM_AIR_LOGS, air_logs ? 1 : 0);
    } else {
        printf("ar8030: the air unit is the stock air app (no kestrel-air announcement in its "
               "version message): latency timed from its packet stamps (whole ms), the "
               "standard radio frame, no Max Bandwidth / Max Bitrate (kestrel-air only)\n");
    }
}

// Open8030 here: the menu's caps on the link. On the stats thread: each try
// is up to a second of SET 22 and two half-second requests, retried every 2 s
// for as long as the radio does not take them - on the video thread (it was in
// bw_poll) a radio that stopped answering held the pictures that long.
void Ar8030Link::send_caps() {
    // The menu's caps (RADIO > Max Bandwidth, Max MCS) on the
    // link - the widest it may go (SET 22, what our radio asks the air's for;
    // live, no restart) and the highest MCS our receive policy may use
    // (request 0x0c5a0036). Our radio keeps both until changed; sent again at
    // every link-up all the same. Every 2 s until all are taken (SET 22 waits
    // for the link); -95 (a radio without the request) counts as taken.
    // AUTO / off: nothing sent at link-up, the radio runs on its own config;
    // chosen from the menu, sent once (40 MHz, MCS none, steady off) to lift
    // what was set before.
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    const uint64_t t = now_ms();
    if (dev && radio_fw == 1 && caps_due.load() && link_state_.load() == 2 && arlink_request) {
        static uint64_t tried = 0;
        if (t - tried >= 2000) {
            tried = t;
            const bool live = caps_live.load();
            const int mhz = max_bw_mhz;
            int rc_bw = 1, rc_mcs = 1, rc_st = 1;   // 1: not sent
            if (mhz > 0 || live) {
                const uint8_t gear = mhz == 0 || mhz >= 40 ? 5 : mhz >= 20 ? 4 : mhz >= 10 ? 3 : 2;
                bb_set_bandwidth_t p;
                memset(&p, 0, sizeof(p));
                p.slot = BB_SLOT_AP;
                p.dir = BB_DIR_RX;
                p.bandwidth = gear;
                rc_bw = ar_ioctl(dev, BB_SET_BANDWIDTH, &p, NULL);
            }
            uint32_t got = 0;
            if (max_mcs >= 0 || live) {
                const uint8_t cap = max_mcs < 0 ? 0xff : (uint8_t)(int8_t)max_mcs;
                rc_mcs = arlink_request(0x0c5a0036, &cap, 1, nullptr, 0, &got, 500);
            }
            if (steady || live) {
                const uint8_t st = steady ? 1 : 0;
                rc_st = arlink_request(0x0c5a0037, &st, 1, nullptr, 0, &got, 500);
            }
            auto rc = [](int r) { return r == 1 ? std::string("not sent") : std::to_string(r); };
            if (rc_bw != 1 || rc_mcs != 1 || rc_st != 1)
            printf("ar8030: caps: %s (SET 22 -> %s), MCS %s (-> %s), steady latency %s (-> %s)\n",
                   mhz ? ("up to " + std::to_string(mhz) + " MHz").c_str() : "AUTO", rc(rc_bw).c_str(),
                   max_mcs < 0 ? "AUTO" : std::to_string(max_mcs).c_str(), rc(rc_mcs).c_str(),
                   steady ? "on" : "off", rc(rc_st).c_str());
            auto taken = [](int r) { return r == 0 || r == 1 || r == -95; };
            if (taken(rc_bw) && taken(rc_mcs) && taken(rc_st)) { caps_due.store(false); caps_live.store(false); }
        }
    }
}

// The stock "enable I-frames" message, which the handshake also carries.
void Ar8030Link::request_air_idr() {
    if (ctrl_sockfd < 0) return;
    std::vector<uint8_t> f = sky_proto.build_u8(sky::CMD_ENABLE_IDR, 1);
    int w = bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 200);
    printf("ar8030: asked the air unit for a keyframe (seq=0x%02X) -> %d\n", f[3], w);
}

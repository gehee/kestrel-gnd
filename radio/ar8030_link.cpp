// The AR8030 link: its construction, what it is as a radio::Link (the
// commands carried out between its steps, what it holds), and its loop.
#include "radio/ar8030_link_detail.hpp"
#include "radio/native_pairing.hpp"
#include "base/shell.hpp"

using namespace radio;


Ar8030Link::Ar8030Link(radio::Model& model, const Ar8030Config& cfg,
                       Settings& settings, const VideoCodec& codec_, std::shared_ptr<Vdec> vdec_,
                       std::atomic<bool>* stop_signal,
                       std::string host_, int host_port_,
                       int slot_, int video_port_)
    : native_sdio_(cfg.native_sdio), pairing_file_(cfg.pairing_file), peer_(cfg.peer), model_(model), settings_(settings), fc_out_(model.fc()), imu_out_(model.imu()),
      video_("ar8030", codec_, vdec_, cfg.decode_enabled, cfg.stream_decode, model, *this),
      codec(codec_), vdec(vdec_), should_stop(stop_signal),
      host(std::move(host_)), host_port(host_port_), slot(slot_), video_port(video_port_),
      freq_khz(cfg.freq_khz), bandwidth(cfg.bandwidth), do_pair(cfg.do_pair),
      ap_index(cfg.ap_index), chan_auto(cfg.chan_auto), chan_manual_cli(cfg.chan_manual_cli),
      chan_hop(cfg.chan_hop), chan_fixed_mhz(cfg.chan_fixed_mhz), tx_power_dbm(cfg.tx_power_dbm),
      tx_power_mw(cfg.tx_power_mw), tx_power_want_mw(cfg.tx_power_want_mw),
      air_floor_us(cfg.air_floor_us), skip_handshake(cfg.skip_handshake),
      standby_mode(cfg.standby_mode), prj_rf_bw(cfg.prj_rf_bw), prj_rf_pwr_mw(cfg.prj_rf_pwr_mw),
      air_bw(cfg.air_bw), max_kbps(cfg.max_kbps), max_exp_us(cfg.max_exp_us), strips(cfg.strips), air_logs(cfg.air_logs),
      stream_decode(cfg.stream_decode), max_bw_mhz(cfg.max_bw_mhz), max_mcs(cfg.max_mcs),
      steady(cfg.steady), radio_fw(cfg.radio_fw), frame_mode(cfg.frame_mode),
      replay_stock_rf(cfg.replay_stock_rf), msp_bb_port(cfg.msp_bb_port),
      dump_path(cfg.dump_path), replay_path(cfg.replay_path),
      video_dump_path(cfg.video_dump_path), decode_enabled(cfg.decode_enabled),
      mode_w(cfg.mode_w), mode_h(cfg.mode_h), mode_fps(cfg.mode_fps), mode_chn(cfg.mode_chn),
      probe_modes(cfg.probe_modes), probe_rf(cfg.probe_rf) {
    if (native_sdio_ && !peer_) peer_ = radio::load_native_peer(pairing_file_);
    accum.reserve(AR_READ_CHUNK * 2);
    link_st_last_.bw_step = -1;          // not known, as each publish starts it
}

Ar8030Link::~Ar8030Link() { disconnect_bb(); }

// ---- radio::Link ----------------------------------------------------------------

// The commands, on the radio thread (Model::take_commands), in the order they
// were asked: the radio's values are written by its own threads only. The
// model saved them as they were asked (Model::save); a setting the radio
// loads with restarts it here.
void Ar8030Link::apply(const radio::Command& c) {
    using namespace radio;
    static_assert(std::variant_size_v<Command> == 16, "a new command: carry it out here (and save it in Model::save if it is a setting)");
    if (const auto* a = std::get_if<cmd::AirLogs>(&c)) {
        air_logs = a->on;
        request_setting(CAM_AIR_LOGS, a->on);
    } else if (const auto* a = std::get_if<cmd::CameraSetting>(&c)) {
        request_setting(a->field, a->value);
    } else if (const auto* a = std::get_if<cmd::RfControl>(&c)) {
        // A channel picked on the channel page is FIXED from here on
        // (chan_fixed_mhz); AUTO HOP is off with it.
        if (a->fixed_mhz >= 0) {
            chan_fixed_mhz = a->fixed_mhz;
            if (a->fixed_mhz) chan_hop = 0;
        }
        request_rf(a->field, a->value);
    } else if (const auto* a = std::get_if<cmd::MaxBitrate>(&c)) {
        max_kbps = a->kbps;
        request_setting(CAM_MAX_KBPS, a->kbps);
    } else if (const auto* a = std::get_if<cmd::MaxExposure>(&c)) {
        max_exp_us = a->us;
        request_setting(CAM_MAX_EXP_US, a->us);
    } else if (const auto* a = std::get_if<cmd::Strips>(&c)) {
        // Asked now of an air unit that has them; its info re-asks while it
        // says otherwise.
        strips = a->n;
        if (air_strips_max_ > 1) request_setting(CAM_STRIPS, strips_wanted());
    } else if (const auto* a = std::get_if<cmd::MaxBandwidth>(&c)) {
        // Open8030 here takes it live (SET 22), 5 to 40 MHz. Stock: the
        // goggle's radio config caps the width (bb_only.sh picks it from the
        // setting), 20 or 40, so a change restarts the radio.
        if (radio_fw == 1) {
            max_bw_mhz = a->mhz;
            request_setting(CAM_MAX_BW, a->mhz == 0 || a->mhz >= 40 ? 40 : 20);
            caps_now(true);
            region_changed();   // AUTO's channels are judged within this cap
        } else {
            const int bw = a->mhz == 20 ? 20 : 40;
            const int was = max_bw_mhz;
            max_bw_mhz = bw;
            request_setting(CAM_MAX_BW, bw);
            if (bw != was) restart_with(radio::setting::ar8030_max_bw_mhz, bw, "max bandwidth");
        }
    } else if (const auto* a = std::get_if<cmd::Firmware>(&c)) {
        // Which baseband firmware bb_only.sh uploads with the radio (0 stock,
        // 1 Open8030): loaded with it, so a change restarts it.
        const int was = radio_fw;
        radio_fw = a->firmware;
        if (a->firmware != was) restart_with(radio::setting::ar8030_firmware, a->firmware, a->firmware ? "radio firmware Open8030" : "radio firmware stock");
    } else if (const auto* a = std::get_if<cmd::FrameMode>(&c)) {
        frame_mode = a->mode;
        frame_mode_changed.store(true);
    } else if (const auto* a = std::get_if<cmd::MaxMcs>(&c)) {
        max_mcs = a->mcs;
        caps_now(true);
    } else if (const auto* a = std::get_if<cmd::Steady>(&c)) {
        steady = a->on;
        caps_now(true);
    } else if (const auto* a = std::get_if<cmd::ChannelHop>(&c)) {
        // No longer FIXED; AUTO's pick is made now.
        chan_fixed_mhz = 0;
        chan_hop = a->hop;
        region_pick = true;
        region_changed();
    } else if (const auto* a = std::get_if<cmd::VideoMode>(&c)) {
        // The camera is reached over the baseband's port-2 control socket
        // (SET_CHN_RES), and it has its own list of modes.
        request_mode_index(a->index);
    } else if (std::holds_alternative<cmd::Bind>(c)) {
        request_bind();
    } else if (std::holds_alternative<cmd::RegionChanged>(c)) {
        region_changed();
    } else if (const auto* a = std::get_if<cmd::ScanScreen>(&c)) {
        scan_active = a->open;   // the stats thread polls the scan fast while it is open
    }
}

radio::State Ar8030Link::held() const {
    radio::State s;
    s.radio_fw = radio_fw;
    s.tx_power_mw = tx_power_mw;
    s.tx_power_want_mw = tx_power_want_mw;
    s.max_mcs = max_mcs;
    s.frame_mode = frame_mode;
    s.max_kbps = max_kbps;
    s.max_exp_us = max_exp_us;
    s.strips = strips;
    s.max_bw_mhz = max_bw_mhz;
    s.steady = steady;
    s.chan_auto = chan_auto;
    s.chan_hop = chan_hop;
    s.bind_state = bind_state.load();
    s.bind_secs_left = bind_secs_left.load();
    s.bind_peer = bind_peer.load();
    s.resend_known = resend_known_.load();
    return s;
}

// The goggle's radio config is loaded with the radio (bb_only.sh reads the
// saved settings), so a change to it restarts the radio, and kestrel-gnd with
// it (the driver cannot go while kestrel-gnd holds it) - from a shell of its
// own. The setting is saved here as the radio takes it (a stock radio's
// width is 20 or 40), and written out before the restart.
void Ar8030Link::restart_with(const Setting<int>& setting, int value, const char* why) {
    if (native_sdio_) { printf("ar8030: %s requires a native radio restart\n", why); return; }
    printf("ar8030: %s - restarting the radio with it\n", why);
    fflush(stdout);
    settings_.set(setting, value);
    settings_.flush();
    if (run_shell("setsid sh -c '/etc/init.d/S70kestrel stop; /etc/init.d/S60ar8030 restart; "
               "sleep 2; /etc/init.d/S70kestrel start' </dev/null >/dev/null 2>&1 &") != 0)
        printf("ar8030: could not restart the radio\n");
}
// Default OFF. Replaying stock's cmd138(4, 500) is accepted (rc=0) but on its
// own it COSTS throughput: measured 3.06 -> 2.09 Mbps at the USB layer, MCS 10
// -> 8, which is within noise of the BB_SET_BANDWIDTH(20M) result (2.10). Both
// widen the ground receiver while the air unit keeps transmitting narrow, so
// the extra noise bandwidth just lowers SNR. Enable only together with the
// air-side half below.

void Ar8030Link::request_rf(int field, int value) {
    std::lock_guard<std::mutex> lk(pending_mtx);
    pending_rf.push_back({field, value});
}

void Ar8030Link::request_setting(int field, int value) {
    std::lock_guard<std::mutex> lk(pending_mtx);
    pending_settings.push_back({field, value});
}

void Ar8030Link::run() {
    // stdout is block-buffered when redirected to a file; line-buffer it so
    // the connect/socket diagnostics below actually reach the log.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    SchedulingHelper::set_thread_params_max_realtime("AR8030_RX", 20);
    printf("ar8030: source starting (host=%s:%d slot=%d port=%d) decode=%s\n",
           host.c_str(), host_port, slot, video_port,
           decode_enabled ? "ON" : "off (--ar8030-decode to enable)");
    if (!replay_path.empty()) { run_replay(); return; }

    std::vector<uint8_t> buf(AR_READ_CHUNK);

    while (!*should_stop) {
        bb_watchdog_alive();   // the loop is still going round
        if (air_info_clear_.exchange(false)) clear_air_info();   // the link went down (on_link_event)
        model_.take_commands(*this);   // the menu's, since the last pass
        bw_poll();
        if (sockfd < 0) {
            if (!connect_bb()) {
                disconnect_bb();
                if (*should_stop) break;
                if (++connect_fails == 1 || connect_fails % 10 == 0)
                    printf("ar8030: connect attempt %d failed, retrying\n", connect_fails);

                // Escalate. Retrying by itself never clears a daemon that has
                // stopped answering BB_INIT_REQ: measured, every attempt fails
                // the same way, because the fault is on its side - a previous
                // client died holding the owner slot and nothing hands it back.
                // Restarting the daemon is the only thing that does, which is
                // also why a bare re-exec of ourselves was never enough.
                //
                // Not on the first failure. At boot S60ar8030 backgrounds its
                // own bring-up, so an early attempt can fail simply because the
                // daemon is not up yet, and restarting it then would push the
                // thing we are waiting for further away.
                if (!native_sdio_ && connect_fails >= kFailsBeforeDaemonRestart &&
                    (connect_fails % kFailsBeforeDaemonRestart) == 0) {
                    // On ar_libre this restarts the baseband itself: arlink.ko
                    // is unloaded and the radio power-cycled (bb_only.sh).
                    printf(on_ar_libre() ? "ar8030: %d bring-ups failed - restarting the baseband\n"
                                         : "ar8030: %d bring-ups failed - restarting the daemon\n",
                           connect_fails);
                    settings_.flush();   // bb_only.sh reads it
                    if (run_shell("/etc/init.d/S60ar8030 restart >/dev/null 2>&1") != 0)
                        printf("ar8030: S60ar8030 restart returned non-zero\n");
                    // It backgrounds its own bring-up: loading the driver and
                    // opening the device takes seconds, and asking too early
                    // just burns another failed attempt.
                    for (int i = 0; i < 100 && !*should_stop; i++) { usleep(100000); model_.take_commands(*this); }
                }
                for (int i = 0; i < 10 && !*should_stop; i++) { usleep(100000); model_.take_commands(*this); }
                continue;
            }
            connect_fails = 0;
        }

        // Keep the OSD repainting even with no video: its render is driven by
        // frame updates, so an unlinked air unit would otherwise leave the
        // last (empty) frame on screen forever. Pipeline does the same at 1Hz.
        drain_control_socket();
        drain_msp_socket();
        // (The channel scan is the stats thread's: a radio round trip here
        // would stall the video.)

        // A menu mode change parked by the OSD thread.
        int want = pending_mode_index.exchange(-1);
        if (want >= 0 && want < sky::kFpvModeCount) {
            const sky::VideoMode& m = sky::kFpvModes[want];
            if (m.w == sky_cfg.ch0_w && m.h == sky_cfg.ch0_h && m.fps == sky_cfg.ch0_fps) {
                // The mode the air unit runs: no switch (it restarts the encoder).
                printf("ar8030: menu mode %s - already this air unit's, nothing sent\n", m.name);
            } else {
                mode_w = m.w; mode_h = m.h; mode_fps = m.fps; mode_chn = 0;
                printf("ar8030: menu requested mode %s\n", m.name);
                apply_video_mode();
            }
        }
        apply_pending_settings();
        // Rotate taken: the mode the air unit runs, sent again, is what makes
        // the camera turn. Without it the video stopped until the silence
        // check re-sent the handshake, ~3 s later.
        if (remode_after_angle_) {
            remode_after_angle_ = false;
            mode_w = sky_cfg.ch0_w; mode_h = sky_cfg.ch0_h; mode_fps = sky_cfg.ch0_fps; mode_chn = 0;
            printf("ar8030: rotate taken - the camera's mode again, %dx%d@%d\n", mode_w, mode_h, mode_fps);
            apply_video_mode();
        }
        apply_pending_rf();
        chan_follow_poll();
        if (region_due.load() && now_ms() >= region_retry_ms_ && region_due.exchange(false)) region_auto_apply();
        // Binding, if the menu or the front-panel button asked for it. One
        // non-blocking step per pass; it owns the baseband only in short
        // bursts, so video and telemetry keep flowing while it runs.
        bind_step();

        // The radios associate on their own schedule, and after a restart of
        // either end that takes seconds. Everything sent before then - the
        // whole handshake, including the video configuration that makes the
        // air unit stream - goes out over a link with nobody on the far side.
        // That is why the acks time out on every start and why video appears
        // only when the air unit eventually decides to send some. Watch for
        // the link coming up and send it then, when there is something there
        // to hear it.
        {
            uint64_t tl = now_ms();
            {
                // The state comes from the stats thread's poll (400 ms on
                // Artosyn's library, 5 s on ar_libre) and, on ar_libre, from
                // the chip's link-state event the moment it changes. Reading it costs nothing, so every pass.
                int ls = link_state_.load();
                // (The HUD is told by the stats thread and the event handler.)
                if (ls >= 0 && ls != last_link_state) {
                    chan_settle_ms_ = tl + 6000;
                    printf("ar8030: link state %d -> %d (%s)\n", last_link_state, ls,
                           ls == 2 ? "linked" : ls == 1 ? "connecting" : "not linked");
                    // The step to 40 MHz (kestrel-air's, after link-up) makes the
                    // radios re-form the link: down and back within about a
                    // second, the same air unit on the same stream. Taken as a
                    // new link it cost ~4.6 s of picture (resync gate, handshake)
                    // where the radio's own gap is ~0.1 s - and only when this
                    // loop happened to look while it was down. So a drop within
                    // 2 s of a bandwidth change that is back within 3 s keeps
                    // everything; only the picture cut in half goes.
                    if (last_link_state == 2 && ls != 2) {
                        const uint64_t bw = bw_change_ms_.load();
                        retune_down_ms_ = (bw && tl - bw <= 2000) ? tl : 0;
                        if (retune_down_ms_)
                            printf("ar8030: link re-forming after a bandwidth change\n");
                    }
                    const bool retune = ls == 2 && retune_down_ms_ && tl - retune_down_ms_ <= 3000;
                    if (ls == 2) retune_down_ms_ = 0;
                    // A channel pinned from the menu holds only while the link
                    // does. The air unit decides the channel - after a restart
                    // it is back on its own - and a ground left fixed on the
                    // pinned one never finds it: video stayed gone ~100 s, until
                    // the idle reconnect below happened to reset the mode. So a
                    // lost link puts the ground back to searching, as at start.
                    // --ar8030-chan-manual asked to stay fixed, and does.
                    if (last_link_state == 2 && ls != 2 && !retune_down_ms_ && !lost_reopened_) link_lost_ms_ = tl;
                    if (ls == 2) { link_lost_ms_ = 0; lost_reopened_ = false; }
                    if (last_link_state == 2 && ls != 2 && !retune_down_ms_ && !chan_auto && !chan_manual_cli && bb_dev) {
                        printf("ar8030: link lost on a pinned channel - searching again\n");
                        search_all_channels();
                    }
                    if (retune) {
                        drop_pictures();
                        printf("ar8030: link back after the bandwidth change - same link, video goes on\n");
                    } else if (ls == 2) {
                        // Whatever is still queued belongs to the link that just
                        // ended. Feeding it to the decoder replays history as
                        // fast as it arrives - the picture races, which is worse
                        // than no picture at the moment you are trying to work
                        // out where the aircraft is. Drop it and wait for a
                        // keyframe from the new link.
                        drop_pictures();
                        live_cap_valid_  = false;   // a new link may be a new clock
                        hdr_stale_       = false;
                        resync_          = true;
                        resync_live_     = 0;
                        resync_prev_us_  = 0;
                        resync_prev_cap_ = 0;
                        resync_start_ms_ = now_ms();
                        air_ver_seen_    = false;   // the next air unit may be the other kind
                        clear_air_info();
                        air_hdr_tag_     = false;
                        air_kestrel_     = false;
                        air_feat_        = 0;
                        air_stock_hdrs_  = 0;
                        model_.update_unit([](air::UnitState &a) { a.stock = false; });
                        frame_clock_.forget_air();
                        printf("ar8030: link up - dropping video until it arrives "
                               "at the rate it was captured\n");
                        printf("ar8030: link up - sending the air handshake now that "
                               "there is a link to carry it\n");
                        send_air_handshake(true);
                        last_rehandshake_ms = tl;
                        rehandshakes = 0;
                    }
                    if (ls == 2 && !frame_mode_due_ms_.load()) frame_mode_due_ms_.store(tl + 1000);   // no event (stock library)
                    last_link_state = ls;
                }
                if (frame_mode_changed.exchange(false)) frame_mode_due_ms_.store(tl);   // the menu
                const uint64_t fm_due = frame_mode_due_ms_.load();
                // Once the air unit has said what it runs (kestrel-air's first
                // tagged header, or the stock app's 200 untagged): a stock air
                // unit keeps the standard frame every link starts on.
                // Cleared only if no link-up moved it since it was read: that
                // one's second is still to come.
                uint64_t fm_was = fm_due;
                if (fm_due && ls == 2 && tl >= fm_due && tl - bw_change_ms_.load() >= 1000 && air_ver_seen_ &&
                    frame_mode_due_ms_.compare_exchange_strong(fm_was, 0)) {
                    if (air_kestrel_) set_frame_mode();
                    else printf("ar8030: stock air app: the radio frame stays standard\n");
                }
            }
        }

        uint64_t now = now_ms();
        if (model_.viewers() && now - last_tick_ms >= 1000) {
            last_tick_ms = now;
            // Only publish an idle (zero) bitrate once video has actually
            // stopped. update_stats() publishes the true figure once a second
            // while data flows, so zeroing unconditionally here made the OSD
            // alternate between the real bitrate and 0 every other second.
            if (now - last_data_ms >= 1500) {
                model_.video_bandwidth(0.0f, (uint32_t)frames_lost);   // idle tick
            }
        }

        // timeout in ms; short enough that *should_stop is honoured promptly.
        // On ar_libre: what is queued, else one wait on the video, control
        // and MSP sockets together, so a slice is read the moment it lands.
        // Read in turn, each with its own timeout, the control and MSP drains
        // above held it up to 1 ms whenever they found nothing (their last
        // read always does): the stats' Dec wait, 0.8 ms (2026-10-12).
        // The video silent while the air unit still talks (see where it is
        // called): the handshake again. Also checked on a pass that only
        // found the control or MSP socket's data - an air unit sending its
        // telemetry every ~0.5 s kept the video read from ever timing out,
        // and this from ever running (2026-10-14, video gone after a step
        // to 40 MHz until kestrel-gnd restarted).
        auto video_silent_check = [&]() {
                uint64_t t_now = now_ms();
                // Tight enough that an air unit coming back on its own power is
                // streaming again inside ten seconds: about three to notice, then
                // the handshake itself.
                // Talking: anything from it in the last 8 s - with the video gone
                // a kestrel-air sends little else than its status, every ~5 s
                // (3 s missed it: video stayed off after a step to 40 MHz).
                bool air_talking = last_ctrl_ms && (t_now - last_ctrl_ms) < 8000;
                // No video yet since this start: counted from the link-up's
                // handshake. A handshake sent just before the link re-formed
                // (the radio's own step to 40 MHz right after link-up, as after
                // RADIO > Max Bandwidth restarts the radio) can be lost, and
                // the air unit then talks but never streams.
                bool video_gone  = last_data_ms ? (t_now - last_data_ms) > 2500
                                                : last_rehandshake_ms && (t_now - last_rehandshake_ms) > 5000;
                // Four tries 6 s apart, then one every 15 s for as long as it
                // talks without video - never none. The count goes back to 0
                // only with video or a link-up, and a kestrel-air
                // restarts on a link that stays up: four spent on one whose
                // camera gave no frames left the next, healthy, without a
                // handshake - no video for minutes, until kestrel-gnd
                // restarted (2026-11-04). Its telemetry kept the idle reads
                // from counting, so the reopen below never came either.
                bool cooled      = (t_now - last_rehandshake_ms) > (rehandshakes < 4 ? 6000u : 15000u);
                if (air_talking && video_gone && cooled) {
                    last_rehandshake_ms = t_now;
                    rehandshakes++;
                    if (rehandshakes <= 4)
                        printf("ar8030: video silent %llums but telemetry still arriving - "
                               "re-sending the air handshake (%d/4)\n",
                               (unsigned long long)(t_now - last_data_ms), rehandshakes);
                    else
                        printf("ar8030: video silent %llums but telemetry still arriving - "
                               "re-sending the air handshake (%d, every 15 s now)\n",
                               (unsigned long long)(t_now - last_data_ms), rehandshakes);
                    send_air_handshake();
                }
        };
        int rd;
        bool gone = false;   // ar_libre: a read failed, or poll() says a socket is gone
        if (poll_fd(sockfd) >= 0) {
            rd = arlink_socket_read_now(sockfd, buf.data(), (uint32_t)buf.size());
            if (rd == 0) {
                struct pollfd p[3];
                int np = 0;
                bool other = false;
                for (int s : { sockfd, ctrl_sockfd, msp_sockfd }) {
                    const int f = poll_fd(s);
                    if (f >= 0) p[np++] = { f, POLLIN, 0 };
                }
                if (poll(p, (nfds_t)np, 500) > 0)
                    for (int i = 0; i < np; i++) {
                        gone |= (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
                        if (i) other |= (p[i].revents & POLLIN) != 0;
                    }
                rd = arlink_socket_read_now(sockfd, buf.data(), (uint32_t)buf.size());
                if (rd == 0 && other && !gone) { video_silent_check(); continue; }   // the control or MSP socket's: drained above, next pass
                if (rd == 0) rd = -1;             // nothing: as a read timeout
                else if (rd < 0) gone = true;
            } else if (rd < 0) {
                gone = true;                      // an error: 0 is nothing queued
            }
        } else {
            rd = bb_socket_read(sockfd, buf.data(), (uint32_t)buf.size(), 500);
        }
        if (rd > 0) {
            read_rx_us = arlink_socket_rx_ns ? arlink_socket_rx_ns(sockfd) / 1000 : 0;
            read_timeouts = 0;
            last_data_ms = now_ms();
            rehandshakes = 0;
            total_bytes += (unsigned long long)rd;
            if (dumped_reads < 5) {
                dumped_reads++;
                char hex[3 * 32 + 1];
                int n = rd < 32 ? rd : 32;
                for (int i = 0; i < n; i++) sprintf(hex + i * 3, "%02X ", buf[i]);
                hex[n * 3] = 0;
                printf("ar8030: read[%d] %d bytes: %s\n", dumped_reads, rd, hex);
            }
            if (ltrace::on()) ltrace::rec(ltrace::kRead, now_us(), (uint32_t)rd, 0);
            // `echo N > /tmp/vdump.req`: the next N seconds of the radio's video
            // bytes, as read, into /tmp/vdump.bin - what --debug-video-dump does,
            // without restarting kestrel-gnd - and the parameter sets held now
            // into /tmp/vdump.ps.
            {
                static uint64_t req_check_ms = 0, dump_until_ms = 0;
                const uint64_t t = now_ms();
                if (t - req_check_ms >= 500) {
                    req_check_ms = t;
                    if (FILE *rq = fopen("/tmp/vdump.req", "r")) {
                        int secs = 0;
                        if (fscanf(rq, "%d", &secs) != 1) secs = 0;
                        fclose(rq);
                        unlink("/tmp/vdump.req");
                        if (secs > 0) {
                            if (video_dump_fp) { fclose((FILE *)video_dump_fp); video_dump_fp = nullptr; }
                            // With intra refresh the parameter sets only come with a
                            // keyframe: the ones we hold, to decode the dump with.
                            if (FILE *ps = fopen("/tmp/vdump.ps", "wb")) {
                                for (const auto &q : video_.param_sets()) fwrite(q.data(), 1, q.size(), ps);
                                fclose(ps);
                            }
                            video_dump_path = "/tmp/vdump.bin";
                            video_dump_written = 0;
                            dump_until_ms = t + (uint64_t)secs * 1000;
                        }
                    }
                }
                if (dump_until_ms && t >= dump_until_ms) {
                    if (video_dump_fp) { fclose((FILE *)video_dump_fp); video_dump_fp = nullptr; }
                    printf("ar8030: video dump done, %llu bytes in %s\n", video_dump_written,
                           video_dump_path.c_str());
                    video_dump_path.clear();
                    dump_until_ms = 0;
                }
            }
            if (!video_dump_path.empty() && video_dump_written < 64u * 1024 * 1024) {
                if (!video_dump_fp) {
                    // Appending after a reconnect, which closes it.
                    video_dump_fp = (void *)fopen(video_dump_path.c_str(),
                                                  video_dump_written ? "ab" : "wb");
                    if (video_dump_fp) printf("ar8030: dumping video to %s\n", video_dump_path.c_str());
                    else video_dump_path.clear();
                }
                if (video_dump_fp) {
                    fwrite(buf.data(), 1, (size_t)rd, (FILE *)video_dump_fp);
                    video_dump_written += (unsigned long long)rd;
                }
            }
            consume(buf.data(), (size_t)rd);
        } else {
            // bb_socket_read() returns -1 both for a plain read timeout (no
            // air unit transmitting) and for a dead socket, so a bare -1 is
            // not a reason to tear the link down. Only reconnect once reads
            // have been failing for long enough that the socket is suspect.
            // A dead socket answers at once, every time: the loop went round
            // as fast as it could, on SCHED_FIFO - 200 times and a reopen, or
            // for a whole bind window, which keeps the radio open. While
            // binding, 20 ms a pass instead (short: one failed read is not
            // yet a dead socket).
            if (gone && bind_busy()) usleep(20000);
            if (read_timeouts == 6 && !link_reported) { link_reported = true; report_link_status(); }
            if (++read_timeouts == 1 || read_timeouts % 40 == 0)
                printf("ar8030: no video yet (%d idle reads) - is the air unit powered?\n",
                       read_timeouts);

            // Video silent while telemetry still arrives. The air unit is
            // powered, linked and answering - it has just stopped streaming,
            // which is what happens when it re-links or restarts and comes
            // back without the video configuration we sent at bring-up. The
            // handshake's two SET_CONFIG frames are that configuration, so
            // sending them again is the whole fix.
            //
            // Waiting for the socket to look dead instead (read_timeouts >=
            // 200) does eventually recover this, but only after ~100s of a
            // blank screen, and by tearing down a link that was never broken.
            video_silent_check();
            // NOT while binding. A bind window has no video by definition -
            // no air unit is associated yet, that is the entire point - so
            // this recovery reads the silence as a dead socket and tears down
            // the link the bind is running on. disconnect_bb() closes the
            // device, which loses BB_SET_PAIR_MODE, so any bind outliving
            // this timer was being killed from inside. Seen on hardware as
            // repeated "shutdown:" and "bring-ups failed - restarting the
            // daemon" in the middle of an open window.
            // A link that was up and went down is searched for by the radio,
            // and that search does not always find the air unit again: drive
            // 2026-10-29 (Open8030 both ends), the link dropped on a clean
            // signal (SNR 23, MCS 8) and stayed down until the 200-read rule
            // below, ~105 s of no picture; the reopen then linked in ~3 s.
            // When the search works it relinks within ~10 s (9.2 and 2.7 s at
            // the edge of range the same evening), so 12 s without the link
            // back reopens - once per loss, the 200-read rule after that.
            const bool lost_long = link_lost_ms_ && now_ms() - link_lost_ms_ >= 12000;
            if ((read_timeouts >= 200 || lost_long) && !bind_busy()) {
                if (lost_long) printf("ar8030: link lost 12 s ago and not back - reopening socket\n");
                else printf("ar8030: link idle too long, reopening socket\n");
                if (lost_long) lost_reopened_ = true;
                link_lost_ms_ = 0;
                disconnect_bb();
                // The stream starts again: accum_base_ moves past what goes, so
                // a packet's end or slice checksum still pending is not looked
                // for in the next socket's bytes (false "damaged" counts).
                accum_base_ += accum.size();
                accum.clear();
                pkt_end_pos_ = 0;
                if (xor_pending_) { xor_pending_ = false; xor_unchecked_++; }
                read_timeouts = 0;
                sleep(1);
            }
        }
    }

    printf("ar8030: source stopped (%llu frames)\n", (unsigned long long)video_.frames_seen());
    disconnect_bb();
}

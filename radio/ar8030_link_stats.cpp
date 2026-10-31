// The AR8030 link's figures: the link's stats (its own thread), the channel
// scan, and the air delay by the radio clock.
#include "radio/ar8030_link_detail.hpp"

using namespace radio;

void Ar8030Link::publish_chan_scan() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev || !model_.viewers()) return;
    // ~14 Hz while the scan screen is open, otherwise every 2 s.
    const uint64_t period = scan_active.load() ? 70 : 2000;
    uint64_t now = now_ms();
    if (last_scan_ms && now - last_scan_ms < period) return;
    last_scan_ms = now;

    // The daemon writes more than sizeof(bb_get_chan_info_out_t) into the output
    // buffer, so hand it a generously oversized one and view the struct through
    // it. Passing the bare struct smashes the stack (the RPC client got away
    // with the same request because it received into a sized vector).
    uint8_t raw[4096];
    memset(raw, 0, sizeof(raw));
    if (ar_ioctl(dev, BB_GET_CHAN_INFO, NULL, raw) != 0) return;
    // The SDK's bb_get_chan_info_out_t declares freq[]/power[] as
    // BB_CONFIG_MAX_CHAN_NUM (32) entries, which is wrong for this response:
    // ar_ldy_gnd's parser (0x9bfc4) indexes power as buf[(j + 128) * 4], i.e.
    // the arrays are 128 entries each and power[] starts at offset 4 + 512.
    // Reading power from offset 132 is what made every channel look maximally
    // busy, and the 260-byte struct is why the reply overflowed the stack.
    struct chan_info_real {
        uint8_t  chan_num, auto_mode, acs_chan, work_chan;
        uint32_t freq[128];
        int32_t  power[128];
    };
    const chan_info_real &ci = *(const chan_info_real *)raw;
    int n = ci.chan_num;
    if (n <= 0) return;
    if (n > 128) n = 128;

    if (n > 64) n = 64;   // gnd::ScanSample carries 64 slots
    gnd::ScanSample sc{};
    sc.chan_num  = n;
    sc.auto_mode = ci.auto_mode;
    sc.acs_chan  = ci.acs_chan;
    sc.work_chan = ci.work_chan;
    for (int i = 0; i < n; i++) {
        sc.freq_mhz[i]  = (int)(ci.freq[i] / 1000);
        sc.power_dbm[i] = (int)ci.power[i];
    }
    // BB_GET_CHAN_INFO's work_chan is NOT an index into this freq[] table: with
    // the link demonstrably on 5839 MHz (BB_GET_STATUS tx_freq) it pointed at
    // 5955/6220 MHz on consecutive reads. Resolve the current channel by
    // matching the real link frequency instead, so the screen can mark it.
    if (link_freq_mhz > 0) {
        for (int i = 0; i < n; i++) {
            if (sc.freq_mhz[i] == link_freq_mhz) { sc.work_chan = i; break; }
        }
    }
    sc.last_update_ms = now;
    // Tests: while /tmp/kestrel-scandump exists, each scan read is printed
    // (MHz:dBm per channel), to compare radio firmwares.
    if (access("/tmp/kestrel-scandump", F_OK) == 0) {
        std::string line = "[scandump]";
        char b[24];
        snprintf(b, sizeof b, " auto=%d acs=%d", ci.auto_mode, ci.acs_chan);
        line += b;
        for (int i = 0; i < n; i++) {
            snprintf(b, sizeof b, " %d:%d", sc.freq_mhz[i], sc.power_dbm[i]);
            line += b;
        }
        printf("%s\n", line.c_str());
    }
    // Only log once the table looks real: right after the socket opens the
    // radio has not populated it yet, which is where "link=0MHz" and absurd
    // dBm values come from (the old RPC client logged the same garbage).
    // Also wait for the real link frequency, otherwise the one-shot log prints
    // the unresolved work_chan and looks wrong even when the screen is right.
    bool plausible = link_freq_mhz > 0 && sc.work_chan < n && sc.freq_mhz[sc.work_chan] > 0;
    if (!scan_logged && plausible) {
        scan_logged = true;
        int busiest = 0;
        for (int i = 1; i < n; i++) if (sc.power_dbm[i] > sc.power_dbm[busiest]) busiest = i;
        printf("[scan] %d channels, link=%dMHz, busiest=%dMHz@%ddBm (%s)\n",
               n, (sc.work_chan < n ? sc.freq_mhz[sc.work_chan] : 0),
               sc.freq_mhz[busiest], sc.power_dbm[busiest],
               sc.auto_mode ? "AUTO/ACS" : "MANUAL");
    }
    model_.scan(sc);
}

void Ar8030Link::publish_link_stats() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev || !model_.viewers()) return;

    gnd::LinkStats st{};
    st.bw_step = -1;          // 0 is a real gear (1.25 MHz), so -1 = unknown
    auto to_db = [](uint16_t s) { return s ? 10.0f * log10f((float)s / 36.0f) : 0.0f; };

    bb_get_1v1_info_in_t info_in;
    memset(&info_in, 0, sizeof(info_in));
    info_in.frame_num = 64;
    // Oversized like the CHAN_INFO buffers: the chip's reply is 216 bytes, 8
    // more than our header's struct declares, and the client library copies
    // all of it.
    union {
        bb_get_1v1_info_out_t info;
        uint8_t raw[256];
    } info_buf;
    memset(&info_buf, 0, sizeof(info_buf));
    bb_get_1v1_info_out_t &info = info_buf.info;
    if (ar_ioctl(dev, BB_GET_1V1_INFO, &info_in, &info_buf) == 0) {
        st.snr        = to_db(info.self.snr);
        stat_snr_x10_.store((int)(st.snr * 10.0f + 0.5f));
        stat_gains_.store(info.self.gain_a | info.self.gain_b << 8);
        st.snr_raw    = info.self.snr;
        st.mcs        = info.self.tx_mcs;
        st.power      = info.self.tx_power;
        // The air VTX power we commanded (cmd 0x22), for the OSD to show instead
        // of the ground uplink readback above. tx_power_mw carries the
        // kArPwrLevels encoding (N, or N+1 for auto-capped-at-N).
        st.air_pwr_mw = tx_power_mw;
        st.gains_cur  = info.self.gain_a;
        st.gains_max  = info.self.gain_b;
        st.fec_error = info.self.ldpc_num_err_ratio / 10000.0f;
        st.fec_total = info.self.ldpc_tlv_err_ratio / 10000.0f;
        st.active     = true;
    }

    bb_get_status_in_t sin;
    memset(&sin, 0, sizeof(sin));
    sin.user_bmp = 0x03FF;
    bb_get_status_out_t sout;
    memset(&sout, 0, sizeof(sout));
    if (ar_ioctl(dev, BB_GET_STATUS, &sin, &sout) == 0) {
        st.state = sout.link_status[0].state;
        st.role  = sout.role;
        snprintf(st.mac, sizeof(st.mac), "%02X:%02X:%02X:%02X",
                 sout.mac.addr[0], sout.mac.addr[1], sout.mac.addr[2], sout.mac.addr[3]);
        snprintf(st.peer_mac, sizeof(st.peer_mac), "%02X:%02X:%02X:%02X",
                 sout.link_status[0].peer_mac.addr[0], sout.link_status[0].peer_mac.addr[1],
                 sout.link_status[0].peer_mac.addr[2], sout.link_status[0].peer_mac.addr[3]);
        static const float bw_mhz_f[] = { 1.25f, 2.5f, 5.f, 10.f, 20.f, 40.f };
        for (int pass = 0; pass < 2 && !st.tx_freq && !st.rx_freq; pass++) {
            for (int i = 0; i < BB_DATA_USER_MAX; i++) {
                const bb_user_status_t &us = sout.user_status[i];
                if (us.tx_status.freq_khz == 0 && us.rx_status.freq_khz == 0) continue;
                if (pass == 0 && !(sout.rt_sbmp & (1 << i))) continue;
                st.tx_freq   = us.tx_status.freq_khz / 1000;
                st.rx_freq   = us.rx_status.freq_khz / 1000;
                // TX side: bb_phy_status_t's header warns the RX copy is not
                // meaningful ("如果是RX端此字段无意义"), and this radio bears
                // that out for frequency - rx_status.freq_khz reads 2100 MHz on
                // a link plainly on 5839. Both copies report the same gear, so
                // it makes no difference here, but tx_status is the one whose
                // other fields match reality.
                int bw_tx = us.tx_status.bandwidth;
                int bw_rx = us.rx_status.bandwidth;
                st.bw_step = (bw_tx >= 0 && bw_tx < 6) ? bw_tx : -1;
                last_bw_rx_idx = (bw_rx >= 0 && bw_rx < 6) ? bw_rx : -1;
                st.active    = true;
                break;
            }
        }
    }

    bb_get_mcs_in_t mcs_in;
    memset(&mcs_in, 0, sizeof(mcs_in));
    mcs_in.dir = BB_DIR_RX; mcs_in.slot = (uint8_t)slot;
    bb_get_mcs_out_t mcs_out;
    memset(&mcs_out, 0, sizeof(mcs_out));
    if (ar_ioctl(dev, BB_GET_MCS, &mcs_in, &mcs_out) == 0) {
        st.rx_mcs_val        = mcs_out.mcs;
        st.rx_data_rate_kbps = mcs_out.throughput;
    }

    st.pwr_auto = tx_power_auto;

    // The video link's own width, where the events have said (ar_libre).
    if (video_bw_idx_.load() >= 0) st.bw_step = video_bw_idx_.load();
    st.frame_us = (float)frame_clock_.local_period();
    st.frame_mode_asked = air_ver_seen_ && !air_kestrel_ ? 0 : frame_mode.load();   // stock air: standard

    last_link_kbps = st.rx_data_rate_kbps;   // for the periodic stat line
    last_bw_idx    = st.bw_step;           // live RF bandwidth gear

    if (st.tx_freq > 0) link_freq_mhz = st.tx_freq;

    // Sky cmd 0x26 asks the air unit for its own measured video delay - the
    // number stock shows, computed where both timestamps share a clock domain
    // so the offset that defeats a ground-side calculation never arises.
    //
    // Currently a DEAD END, kept for the next attempt: the air ACKs 0x26 with
    // a bare "26" and no payload, because its handler is gated on bit 2 of
    // vsend_ctx+0x28 (a flow-control bitmap) and nothing we send sets that bit.
    // Whatever enables it has not been identified.
    //
    // Off by default: it is an unknown command to the air unit and there is no
    // reason to send it every second in normal operation. Set
    // KESTREL_SKY_DELAY_PROBE=1 to resume experimenting.
    {
        static int probe = -1;
        if (probe < 0) {
            const char *e = getenv("KESTREL_SKY_DELAY_PROBE");
            probe = (e && *e == '1') ? 1 : 0;
        }
        if (probe && ctrl_sockfd >= 0) {
            std::vector<uint8_t> f = sky_proto.build_u8(sky::CMD_GET_VIDEO_DELAY, 0);
            int w = bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 200);
            static bool req_logged = false;
            if (!req_logged) {
                req_logged = true;
                printf("ar8030: sent CMD_GET_VIDEO_DELAY (0x26) -> %d bytes\n", w);
            }
        }
    }

    st.last_update_ms = now_ms();
    static bool freq_logged = false;
    if (!freq_logged && st.rx_freq > 0) {
        freq_logged = true;
        printf("[link] BB_GET_STATUS rx_freq=%d MHz tx_freq=%d MHz bw=%s MHz (rx_status gear reads %s)\n",
               st.rx_freq, st.tx_freq, gnd::bw_step_label(st.bw_step),
               gnd::bw_step_label(last_bw_rx_idx));
    }
    model_.link_stats(st);
}

int Ar8030Link::current_rx_mcs() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return -1;
    bb_get_status_in_t in;
    memset(&in, 0, sizeof(in));
    in.user_bmp = 0x03FF;
    bb_get_status_out_t st;
    memset(&st, 0, sizeof(st));
    if (ar_ioctl(dev, BB_GET_STATUS, &in, &st) != 0) return -1;
    if (st.link_status[0].state != 2) return -1;   // only meaningful when linked
    return (int)st.link_status[0].rx_mcs;
}

int Ar8030Link::current_link_state() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return -1;
    bb_get_status_in_t in;
    memset(&in, 0, sizeof(in));
    in.user_bmp = 0x03FF;
    bb_get_status_out_t st;
    memset(&st, 0, sizeof(st));
    if (ar_ioctl(dev, BB_GET_STATUS, &in, &st) != 0) return -1;
    return (int)st.link_status[0].state;
}

void Ar8030Link::report_link_status() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    bb_get_status_in_t in;
    memset(&in, 0, sizeof(in));
    bb_get_status_out_t st;
    memset(&st, 0, sizeof(st));
    if (ar_ioctl(dev, BB_GET_STATUS, &in, &st) != 0) return;
    const bb_link_status_t *ls = &st.link_status[0];
    const char *state = ls->state == 2 ? "CONNECTED"
                      : ls->state == 1 ? "connecting" : "not linked";
    printf("ar8030: role=%u link slot0 state=%u (%s) rx_mcs=%u peer=%02X:%02X:%02X:%02X"
           " local=%02X:%02X:%02X:%02X\n",
           st.role, ls->state, state, ls->rx_mcs,
           ls->peer_mac.addr[0], ls->peer_mac.addr[1], ls->peer_mac.addr[2], ls->peer_mac.addr[3],
           st.mac.addr[0], st.mac.addr[1], st.mac.addr[2], st.mac.addr[3]);
}

void Ar8030Link::start_stats() {
    stop_stats();
    {
        std::lock_guard<std::mutex> g(stats_mu);
        stats_stop = false;
    }
    link_state_.store(-1);
    frame_clock_.set_linked(false);
    stats_thread = std::thread(&Ar8030Link::stats_run, this);
    // Open8030 on our radio: its frame info (the air radio's clock from the
    // frame stamps). A radio that does not answer is asked again only every
    // 30 s, so the frame clock's steps keep their pace.
    if (radio_fw == 1 && arlink_request)
        frame_clock_.set_info([](FrameStampInfo *si) {
            static int fails = 0;
            static uint64_t quiet_until = 0;
            const uint64_t t = now_ms();
            if (t < quiet_until) return -1;
            uint32_t got = 0;
            // A firmware without the later fields (retries, steady mode: Open8030
            // main has the frame stamps without them) answers shorter: they read 0.
            memset(si, 0, sizeof(*si));
            const int rc = arlink_request(0x0c5a0035, nullptr, 0, si, sizeof(*si), &got, 300);
            if (rc == 0 && got >= offsetof(FrameStampInfo, retries) && si->version >= 1) { fails = 0; return 0; }
            if (++fails >= 3) { fails = 0; quiet_until = t + 30000; }
            return -1;
        });
    frame_clock_.start(
        [this](uint8_t *b) {
            bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
            uint8_t in[4] = {0x6e, 0x0e, 1, 0}, out[256] = {0};
            const int rc = dev ? ar_ioctl(dev, 0x01000064, in, out) : -1;
            if (rc == 0) *b = out[0];
            return rc;
        },
        [this](uint64_t t, double *ap) {
            if (!ap_off_valid_.load(std::memory_order_acquire)) return false;
            *ap = (double)t - (double)ap_off_us_.load(std::memory_order_relaxed);
            return true;
        });
}

void Ar8030Link::stop_stats() {
    frame_clock_.stop();
    if (!stats_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> g(stats_mu);
        stats_stop = true;
    }
    stats_cv.notify_all();
    stats_thread.join();
}

void Ar8030Link::stats_run() {
    pthread_setname_np(pthread_self(), "AR8030_STAT");
    // On ar_libre the link state comes from the chip's event, so the poll is
    // only a safety net against a missed one; on Artosyn's library it is the
    // only source.
    const uint64_t link_period = on_ar_libre() ? 5000 : 400;
    uint64_t next_link = 0, next_stats = now_ms() + 3000, last_stats = 0;
    std::unique_lock<std::mutex> lk(stats_mu);
    while (!stats_stop) {
        lk.unlock();
        uint64_t t = now_ms();
        if (t >= next_link) {
            next_link = t + link_period;
            // Published on this timer, not only when a frame arrives: the
            // HUD's status snapshot used to be refreshed solely from
            // update_stats(), which runs on frame arrival - so the moment the
            // air unit went away and the frames stopped, the snapshot froze at
            // "linked" and nothing downstream could ever see the drop.
            //
            // Only the state is refreshed here. Every other figure - SNR,
            // MCS, distance - keeps its last value, which is exactly what
            // IDLE_2 is meant to show: the last reading, held.
            const unsigned events_before = link_events_.load();
            int ls = current_link_state();
            if (ls >= 0 && link_events_.load() == events_before) {   // not overtaken
                link_state_.store(ls);
                frame_clock_.set_linked(ls == 2);
                report_link_state(ls);
            }
        }
        // Every 3 s, or sooner - at most once a second - after an MCS change.
        if (t >= next_stats || (stats_soon_.load() && t - last_stats >= 1000)) {
            stats_soon_.store(false);
            last_stats = t;
            next_stats = t + 3000;
            stat_mcs_.store(current_rx_mcs());
            // BB_CFG_DISTC is refused before the link is up (-2), exactly as
            // BB_SET_BANDWIDTH is (-7). Retry a few times once we are linked.
            int dist_m = current_distance();
            stat_dist_.store(dist_m);
            model_.distance(dist_m);
            publish_link_stats();
        }
        publish_chan_scan();             // its own pace: 2 s, 70 ms while the scan screen is up
        send_caps();                     // when due: after a link-up or a menu change
        // Once a second. It was on the video thread, which then waited for the
        // radio's answer - a lost one held the next picture for a second.
        poll_ap_time();
        lk.lock();
        stats_cv.wait_for(lk, std::chrono::milliseconds(35), [this] { return stats_stop; });
    }
}

void Ar8030Link::update_stats(size_t frame_size) {
    uint64_t t = now_ms();
    // After a gap (the link went, the video stopped) a new period from this
    // frame. The bytes from before it used to be counted as the first second
    // after it: the first line after a link-up said 8-9 Mbit/s "on" the 5 MHz
    // link just come up, which carries 1.56 - the rate from before the drop
    // (2026-10-16 flight log, read as the air unit flooding the new link).
    if (period_start && t - stats_frame_ms_ > 500) {
        period_start = t;
        bytes_received = 0;
    }
    stats_frame_ms_ = t;
    bytes_received += (long long)frame_size;
    if (period_start == 0) { period_start = t; return; }
    if (t - period_start >= 1000) {
        // Over the time the period really took: a second, or a little more.
        double video_bw = (double)bytes_received * 1000.0 / (double)(t - period_start);   // bytes a second
        period_start = t;
        model_.video_bandwidth((float)(video_bw / 125000.0));  // -> Mbit/s
        if (t - last_stat_ms >= 3000) {
            last_stat_ms = t;
            // The radio's figures, as the stats thread last read them.
            int mcs = stat_mcs_.load();
            int dist_m = stat_dist_.load();
            // link_kbps is the SDK's own "theoretical throughput for this
            // slot" (BB_GET_MCS.throughput) - i.e. what the radio believes it
            // can carry. Printing it next to the measured video rate is the
            // only way to tell a link limit from an encoder limit, and every
            // bandwidth theory so far has been argued without it.
            // Cached by publish_link_stats(), on the stats thread.
            printf("ar8030: rx=%llu bytes nals=%llu frames=%llu lost=%llu telemetry_drained=%llu"
                   " msp=%llu mcs=%d bw=%s MHz link=%.2f Mbps video=%.2f Mbps dist=%d"
                   " air=%.1fms stale=%llu foreign=%llu snr=%.1fdB freq=%d gains=%d/%d"
                   " damaged=%llu/%llu\n",
                   total_bytes, nal_count, (unsigned long long)video_.frames_seen(), frames_lost,
                   ctrl_bytes,
                   msp_bytes, mcs >= 0 ? ar_mcs_label(mcs) : -99, gnd::bw_step_label(last_bw_idx.load()), last_link_kbps.load() / 1000.0,
                   video_bw / 125000.0, dist_m,
                   air_delay_ms < 0.0f ? 0.0f : air_delay_ms, stale_pics, video_.foreign_slices(),
                   stat_snr_x10_.load() / 10.0, link_freq_mhz,
                   stat_gains_.load() & 255, stat_gains_.load() >> 8,
                   (unsigned long long)xor_bad_, (unsigned long long)xor_checked_);
        }
        bytes_received = 0;
    }
}

// Capture -> arrival of the picture whose header came last, in us, or 0 until
// there is one. See air_win_min in the header.
uint32_t Ar8030Link::air_delay_for(uint64_t recv_us) {
    if (!cap_valid) return 0;
    const int64_t d = (int64_t)recv_us - (int64_t)cap_us64;
    const uint64_t slot = recv_us / 500000;
    int64_t floor = INT64_MAX;
    for (int i = 0; i < kAirWinSlots; i++)
        if (air_win_slot[i] + kAirWinSlots > slot && air_win_min[i] < floor)
            floor = air_win_min[i];
    // A jump of a second either way is a new clock, not a slow picture: an
    // air unit that rebooted starts its stamps again from zero.
    if (floor != INT64_MAX && (d - floor > 1000000 || d - floor < -1000000)) {
        memset(air_win_slot, 0, sizeof(air_win_slot));
        floor = INT64_MAX;
    }
    int64_t& m = air_win_min[slot % kAirWinSlots];
    if (air_win_slot[slot % kAirWinSlots] != slot) {
        air_win_slot[slot % kAirWinSlots] = slot;
        m = d;
    } else if (d < m) {
        m = d;
    }
    if (d < floor) floor = d;
    const int64_t us = d - floor + air_floor_us;
    if (us <= 0 || us > 1000000) return 0;
    const float ms = (float)us / 1000.0f;
    air_delay_ms = (air_delay_ms < 0.0f) ? ms : 0.95f * air_delay_ms + 0.05f * ms;
    model_.air_delay(air_delay_ms);
    return (uint32_t)us;
}

// One slice's header (hdr[0] = the 0x80 byte), from a kestrel-air that sent
// feature bit 1: bytes 35..41 are the air side's own times, 10 us units, LE.
void Ar8030Link::note_air_latency(const uint8_t *hdr) {
    auto u16 = [&](int o) { return (uint32_t)(hdr[o] | (hdr[o + 1] << 8)) * 10u; };
    lat_enc_.push_back(u16(35));
    lat_queue_.push_back(u16(37));
    lat_write_.push_back(u16(39));
    if (hdr[41] > lat_depth_max_) lat_depth_max_ = hdr[41];
    const uint64_t t = now_ms();
    if (!lat_log_ms_) lat_log_ms_ = t;
    if (t - lat_log_ms_ < 5000) return;
    printf("ar8030: air side, %zu slices in %.1fs: capture->encoder out p50 %.2f p99 %.2f ms | "
           "waiting in the air's ring p50 %.2f p99 %.2f ms | radio write p50 %.2f p99 %.2f ms | "
           "ring depth max %u\n",
           lat_enc_.size(), (t - lat_log_ms_) / 1000.0, pct_ms(lat_enc_, 50), pct_ms(lat_enc_, 99),
           pct_ms(lat_queue_, 50), pct_ms(lat_queue_, 99), pct_ms(lat_write_, 50),
           pct_ms(lat_write_, 99), lat_depth_max_);
    lat_enc_.clear(); lat_queue_.clear(); lat_write_.clear();
    lat_depth_max_ = 0;
    lat_log_ms_ = t;
}

// The radio's AP clock against ours. BB_GET_AP_TIME answers in whole ms, so a
// sample is the middle of the request minus the middle of that ms; a slow
// answer (over 2 ms) says little and is skipped. Averaged (1/8 per sample),
// and started again on a jump of over 20 ms: a new link starts a new clock.
// On the stats thread; the video thread reads the offset.
void Ar8030Link::poll_ap_time() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    const uint64_t t = now_ms();
    if (!dev || t - ap_poll_ms_ < 1000) return;
    ap_poll_ms_ = t;
    uint32_t ap_ms = 0;
    const uint64_t t0 = now_us();
    if (ar_ioctl(dev, BB_GET_AP_TIME, nullptr, &ap_ms) != 0) return;
    const uint64_t t1 = now_us();
    if (t1 - t0 > 2000) {
        ap_poll_ms_ = t - 800;           // again in 200 ms: this thread is not real-time
        return;
    }
    const int64_t off = (int64_t)((t0 + t1) / 2) - ((int64_t)ap_ms * 1000 + 500);
    const int64_t cur = ap_off_us_.load(std::memory_order_relaxed);
    const bool valid = ap_off_valid_.load(std::memory_order_relaxed);
    if (!valid || off - cur > 20000 || off - cur < -20000) {
        if (valid)
            printf("ar8030: the radio's AP clock moved by %.1f ms against ours\n",
                   (off - cur) / 1000.0);
        ap_off_us_.store(off, std::memory_order_relaxed);
        ap_off_valid_.store(true, std::memory_order_release);
    } else {
        ap_off_us_.store(cur + (off - cur) / 8, std::memory_order_relaxed);
    }
}

// Bytes 26..29 of a header: the AP clock (ms) the air unit stamped the slice
// with - when stock built the packet; when kestrel-air's encoder handed the
// slice out, with the sub-ms part in byte 2 (kAirFeatApClock). Our AP clock
// now, minus that, is the stamp -> here. Capture -> stamp comes from the same
// header: kestrel-air's encoder time (bytes 35..36, 10 us), or stock's byte
// 30 (whole ms). Without byte 2 both ends read the clock in whole ms, so a
// picture is good to about 1 ms; with it, to the two offsets' noise.
void Ar8030Link::note_ap_delay(const uint8_t *hdr, size_t len, bool new_pic) {
    if (new_pic) { pic_cap_us_ = 0; pic_air_unknown_ = false; }
    vhdr_.out_us = 0;
    if (len >= 18) {
        const uint32_t h = (uint32_t)(hdr[16] | (hdr[17] << 8)), fps = hdr[9];
        // A new video mode (100 <-> 60 fps is 2 <-> 4 slices at the same size):
        // where a picture's slices start has to be learned again.
        if (hdr_fps_ && hdr_height_ && (fps != hdr_fps_ || h != hdr_height_)) {
            printf("ar8030: video mode %ux%u fps -> %u lines %u fps: learning its slices again\n",
                   hdr_height_, hdr_fps_, h, fps);
            video_.relearn();
            air_readout_us_ = 0;   // the air unit says the new mode's within 2 s
        }
        hdr_height_ = h;
        hdr_fps_ = fps;
        if (fps >= 10 && fps <= 500) {
            g_camera_lines.store((int)h, std::memory_order_relaxed);
            g_camera_fps.store((int)fps, std::memory_order_release);
        }
    }
    if (!ap_off_valid_.load(std::memory_order_acquire) || len < 31) return;
    auto u16 = [&](int o) { return (int64_t)(hdr[o] | (hdr[o + 1] << 8)) * 10; };
    const bool ka = air_kestrel_ && (air_feat_ & kAirFeatLatInfo) && len >= 42;
    const bool fine = ka && (air_feat_ & kAirFeatApClock);
    const uint32_t rt = (uint32_t)hdr[26] | ((uint32_t)hdr[27] << 8) |
                        ((uint32_t)hdr[28] << 16) | ((uint32_t)hdr[29] << 24);
    const uint64_t here = now_us();
    // The radio clock now: on the frame's edges when known (both ends read
    // it there, radio/frame_clock.hpp), else our coarse offset to it.
    uint64_t ap_us;
    const bool on_frame = frame_clock_.ap_us(here, &ap_us);
    if (!on_frame) ap_us = here - (uint64_t)ap_off_us_.load(std::memory_order_relaxed);
    const int64_t frac_us = fine ? ((int64_t)hdr[2] * 1000 + 128) / 256 : 500;
    const int64_t d = (int64_t)(int32_t)((uint32_t)(ap_us / 1000) - rt) * 1000 +
                      (int64_t)(ap_us % 1000) - frac_us;
    if (d < -1000000 || d > 1000000) return;
    // Right after the frame clock counts its edges from our coarse offset,
    // which can be a frame or more off: the fastest of a few hundred slices
    // gets here 2.4 ms to 2.4 ms and a frame after leaving the encoder, or our
    // count is whole frames off. No sooner: the write into the air's radio
    // (~1.3 ms) and our radio's hand-up (~1.7) alone take 3; seen 3.6-4.1 ms on
    // the 3.25 ms frame (40 MHz, mode 2), 5.1-5.9 at 20 MHz (a block written
    // just in time, its airtime, the radio's lead and hand-over). The window
    // was half a frame to one and a half, which on 3.25 ms ends at 4.9.
    // With Open8030 at both ends the fastest comes sooner: 1.93 ms seen on the
    // 3.25 ms frame (2026-10-18), and a floor of 2.4 moved a right count a frame
    // off. 1.5 ms holds both: one frame from it covers stock's 3.6-4.1 there,
    // 4.65-4.92 on 4650.88 and 5.1-5.9 on 5633.92.
    static constexpr double kFastestSliceUs = 1500;
    // 2 s windows, all the time: the first after a count is acted on alone,
    // later ones when two in a row say the same (one window's fastest can be
    // a stray). After a shift, nothing until the frame clock has taken it:
    // a window across the shift held slices from both sides, and the old
    // side asked for the same shift again (2026-10-18: +217 frames twice,
    // then a window on the right side for a moment said all was well - the
    // clock was left 705 ms off).
    if (frame_clock_.count_gen() != fc_count_gen_) {
        fc_count_gen_ = frame_clock_.count_gen();
        fc_verified_ = false;
        fc_fresh_ = true;
        fc_last_k_ = 0;
        fc_min_d_ = INT64_MAX;
        fc_n_ = 0;
        fc_win_ms_ = 0;
    }
    if (fc_wait_ && frame_clock_.shifts_taken() != fc_shifts_seen_) {
        fc_wait_ = false;
        fc_min_d_ = INT64_MAX;
        fc_n_ = 0;
        fc_win_ms_ = 0;
    }
    // Only slices that waited under 1 ms in the air unit's queue: on a slow
    // link they queue there, and the fastest is late for real.
    const int64_t queued = ka ? u16(37) : 0;
    if (on_frame && fine && !fc_wait_ && queued < 1000) {
        fc_min_d_ = std::min(fc_min_d_, d);
        fc_n_++;
        const uint64_t tm = here / 1000;
        if (!fc_win_ms_) fc_win_ms_ = tm;
        if (tm - fc_win_ms_ >= 2000) {
            const double T = frame_clock_.frame_period();
            if (fc_n_ >= 100 && T > 0) {
                const int64_t k = (int64_t)std::floor(((double)fc_min_d_ - kFastestSliceUs) / T);
                // On the frame stamps nothing is counted: a slice out of the
                // window is said, never acted on.
                const bool act = k && (fc_fresh_ || k == fc_last_k_) && !frame_clock_.exact();
                if (k || fc_fresh_)
                    printf("ar8030: frame clock: fastest slice %.2f ms%s\n", fc_min_d_ / 1000.0,
                           act ? ", so our count of edges is off"
                           : k && frame_clock_.exact() ? " (on the frame stamps: left alone)"
                           : k ? " (once: checked again)" : "");
                if (act) {
                    fc_shifts_seen_ = frame_clock_.shifts_taken();
                    fc_wait_ = true;
                    frame_clock_.shift(-k);
                    fc_last_k_ = 0;
                    fc_verified_ = false;
                } else {
                    fc_last_k_ = k;
                    if (!k) fc_verified_ = true;
                }
                fc_fresh_ = false;
            }
            fc_min_d_ = INT64_MAX;
            fc_n_ = 0;
            fc_win_ms_ = tm;
        }
    }
    const int64_t enc = ka ? u16(35) : (int64_t)hdr[30] * 1000;
    // With kestrel-air's frame (the frame clock in play), its figures only
    // once the count is checked: before that our count can be frames off (a
    // first count from our radio's copy of the radio clock, 43-750 ms off
    // under Open8030), and a wrong figure is worse than none.
    if (frame_clock_.air_known() && !(on_frame && (fc_verified_ || frame_clock_.exact()))) {
        pic_air_unknown_ = true;
        return;
    }
    const int64_t queue = ka ? u16(37) : 0;
    if (ka && (hdr[32] & 0x40)) ap_enc_last_.push_back((uint32_t)enc);
    // Not while a link-up's resync drops the pictures: those slices sat in
    // the air radio while the link formed and are never shown, and the
    // per-picture figures leave them out too. Counted, they made the first
    // window after a link-up read p99 765-795 ms with its shown pictures at
    // p99 25-30 (2026-10-20, Open8030 frame stamps: exact from link-up).
    if (!resync_ && d - queue >= 0) {
        ap_link_.push_back((uint32_t)(d - queue));
        if (len >= 33) {   // byte 32: slice index | 0x20 more follow | 0x40 last
            const int pos = (hdr[32] & 0x40) ? 2 : (hdr[32] & 0x1f) == 0 ? 0 : 1;
            ap_link_pos_[pos].push_back((uint32_t)(d - queue));
        }
    }
    // kestrel-air's stamp is when the encoder handed this slice out; stock's,
    // when it built the packet - just after, to the whole ms.
    if (d >= 0 && (uint64_t)d < here) vhdr_.out_us = here - (uint64_t)d;
    if (new_pic && d + enc > 0 && (uint64_t)(d + enc) < here) {
        pic_cap_us_ = here - (uint64_t)(d + enc);
        pic_enc_us_ = (uint32_t)enc;
    }
}

// The AR8030 link's baseband session: the bring-up and teardown through
// the vendor client library, its events, its control and MSP sockets, the
// probes.
#include "radio/ar8030_link_detail.hpp"

using namespace radio;

// One request at a time: the stats thread and this one both talk to the
// baseband, and Artosyn's client library is not known to take concurrent
// requests (ar_libre's does).
static std::mutex g_bb_req_mu;
// Requests waiting for it. A std::mutex hands over to no one: the frame
// clock's search (1.5 s of reads back to back) took it again before a waiting
// thread had woken, and the radio thread's requests and the stats' waited out
// the search. Its reads (ar_ioctl_yield) let one waiting go first.
static std::atomic<int> g_bb_req_waiting{0};

int ar_ioctl(bb_dev_handle_t *dev, uint32_t request, const void *in, void *out) {
    int rc;
    {
        g_bb_req_waiting.fetch_add(1);
        std::lock_guard<std::mutex> one_at_a_time(g_bb_req_mu);
        g_bb_req_waiting.fetch_sub(1);
        rc = bb_ioctl_ex(dev, request, in, out, kIoctlTimeoutMs);
    }
    if (rc != 0) {
        // Rate-limited: a wedged link would otherwise fill the log with these.
        static uint64_t last_ms = 0;
        static unsigned suppressed = 0;
        uint64_t now = now_ms();
        if (now - last_ms > 5000) {
            // rc -2 and -7 are the baseband refusing a command it is not ready
            // for (BB_CFG_DISTC before the link is up, BB_SET_BANDWIDTH early)
            // and are expected; a timeout is rc from pthread_cond_timedwait and
            // means the daemon never answered. Both are worth seeing, but only
            // the second is the fault this timeout exists to convert.
            printf("ar8030: ioctl 0x%X returned %d%s\n", request, rc,
                   suppressed ? " [+more]" : "");
            last_ms = now;
            suppressed = 0;
        } else {
            suppressed++;
        }
    }
    return rc;
}

int ar_ioctl_yield(bb_dev_handle_t *dev, uint32_t request, const void *in, void *out) {
    for (int i = 0; i < 100 && g_bb_req_waiting.load() > 0; i++) usleep(50);   // a few ms at most
    return ar_ioctl(dev, request, in, out);
}

// Events stock subscribes to before touching a socket (capture: class 03 01,
// args 01, 00, 02, 08, 0C). 0=LINK_STATE 1=MCS_CHANGE 2=CHAN_CHANGE
// 8=PRJ_DISPATCH2 (custom dispatch over USB RPC) 12=MAX.
static void ar_event_cb(void *arg, void *user) {
    (void)arg;
    (void)user;   // we only need the subscription to exist, not to act on it
}

// On ar_libre an event's arg is its payload as the chip sent it, which is
// what these decode; what Artosyn's library passes is not known, so there the
// subscriptions stay as they were.
static void ar_event_link_cb(void *arg, void *user) {
    if (arg && user) static_cast<Ar8030Link*>(user)->on_link_event((const uint8_t*)arg);
}
static void ar_event_mcs_cb(void *arg, void *user) {
    if (arg && user) static_cast<Ar8030Link*>(user)->on_mcs_event((const uint8_t*)arg);
}
template <int EV>
static void ar_event_log_cb(void *arg, void *user) {
    if (arg && user) static_cast<Ar8030Link*>(user)->on_logged_event(EV, (const uint8_t*)arg);
}

// Link state (event 0): slot, new state, old state - 0 not linked, 1
// connecting, 2 linked. Acted on at once; the stats thread's poll stays the
// safety net, and drops an answer that an event overtook.
void Ar8030Link::on_link_event(const uint8_t* p) {
    if (p[0] != (uint8_t)slot || p[1] > 2) return;
    link_events_.fetch_add(1);
    link_state_.store(p[1]);
    frame_clock_.set_linked(p[1] == 2);
    if (p[1] == 2) frame_mode_due_ms_.store(now_ms() + 1000);   // see set_frame_mode()
    // A link starts on the config's 5 MHz (gear 2) and widens from there, each
    // step an event 12. A drop that a bandwidth change caused re-forms the same
    // link on its new width, so it keeps the gear.
    if (p[1] == 2 && video_bw_idx_.load() < 0) video_bw_idx_.store(2);
    else if (p[1] == 0 && now_ms() - bw_change_ms_.load() > 2000) {
        video_bw_idx_.store(-1);
        link_bw_mhz = 20;          // the next link starts narrow
        air_prj.store(0);          // the next air unit says what it is
        model_.update_air_radio([](air::RadioState& r) { r.prj = 0; });
        // On the radio thread, which reads and writes what it clears (this is
        // the radio library's event thread).
        air_info_clear_.store(true);
    }
    printf("ar8030: event: link state %u -> %u\n", p[2], p[1]);
    if (p[1] == 2) frame_clock_.reframe();   // a new link, its own frame
    if (p[1] == 2) caps_now(false);          // the menu's caps again on it
    report_link_state(p[1]);
}

// The link's state to the bus, from the chip's event (the radio thread) or
// the stats thread's poll. At every link-up the FC's screen starts over: its
// DisplayPort layer does not reliably resend every element after a real RF
// dropout, and cells held from before the drop would stay on screen forever
// if the reconnect's writes land somewhere else, or simply fewer of them. A
// clean grid costs one blank frame; a screen still showing the last
// connection's numbers costs nobody noticing they are stale.
void Ar8030Link::report_link_state(int ls) {
    if (ls == 2) {
        if (!fc_linked_.exchange(true)) fc_out_.new_session();
    } else {
        fc_linked_.store(false);
    }
    model_.link_state(ls);
}

// MCS change (event 1): slot, direction (1 receive, 0 transmit), new, old -
// confirmed against the polled MCS and the chip's own log ("Slot 0 RX MCS 8
// -> 10"). The radio moves between rungs about once a second while linked,
// which the 3 s poll never showed. The receive MCS goes to the HUD at once,
// and the stats thread is asked to refresh the rest (the data rate) early.
void Ar8030Link::on_mcs_event(const uint8_t* p) {
    if (p[0] != (uint8_t)slot || p[1] != 1 || p[2] > 15) return;
    stat_mcs_.store(p[2]);
    stats_soon_.store(true);
    model_.rx_mcs(p[2]);
}

// Channel change (2) and bandwidth change (12, {0, 1, new gear, old gear}):
// logged. A bandwidth change is also timed - the radio re-forms the link to
// change to 40 MHz, and that brief drop is not a new link (see the video loop).
void Ar8030Link::on_logged_event(int ev, const uint8_t* p) {
    // The distance (Open8030, event 15): {slot, 1 known / 0 not, metres low,
    // high}, pushed as it changes - up to ten a second, so not logged; the
    // stats thread shows the latest every pass and stops asking for it.
    if (ev == 15) {
        const int m = p[1] ? (int)(p[2] | p[3] << 8) : -1;
        stat_dist_.store(m);
        dist_pushed_ms_.store(now_ms());
        return;
    }
    printf("ar8030: event %d: %02X %02X %02X %02X\n", ev, p[0], p[1], p[2], p[3]);
    // TX power (Open8030, event 14): {slot, 0 the air unit's / 1 ours, dBm
    // now, dBm before}, 0 = not known. Shown at once.
    if (ev == 14) {
        if (p[1] == 0) air_live_dbm_ = p[2]; else gnd_live_dbm_ = p[2];
        model_.update_air_radio([&](air::RadioState& r) { r.live_dbm = air_live_dbm_; });
        model_.update_ground([&](gnd::RadioState& r) { r.live_dbm = gnd_live_dbm_; });
        return;
    }
    if (ev == 12) {
        bw_change_ms_.store(now_ms());
        if (p[2] <= 5) video_bw_idx_.store(p[2]);
        // AUTO's channels are judged at the link's width: a step to or from
        // 40 MHz changes which are allowed.
        const int was = link_bw_mhz.load();
        link_bw_mhz = p[2] == 5 ? 40 : 20;
        if (link_bw_mhz.load() != was) region_changed();
        // Only a step to or from 40 MHz (gear 5) changes the radio's frame: 5,
        // 10 and 20 MHz share theirs, and a search across a step only fails.
        if (p[2] == 5 || p[3] == 5) frame_clock_.reframe();
    }
}

void Ar8030Link::subscribe_events() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    static const int evts[] = { 1, 0, 2, 8, 12 };   // stock's order
    for (size_t i = 0; i < sizeof(evts) / sizeof(evts[0]); i++) {
        bb_set_event_callback_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = (bb_event_e)evts[i];
        ev.callback = ar_event_cb;
        ev.user = NULL;
        if (on_ar_libre()) {
            ev.user = this;
            switch (evts[i]) {
            case 0:  ev.callback = ar_event_link_cb; break;
            case 1:  ev.callback = ar_event_mcs_cb; break;
            case 2:  ev.callback = ar_event_log_cb<2>; break;
            case 12: ev.callback = ar_event_log_cb<12>; break;
            default: ev.user = NULL; break;
            }
        }
        printf("ar8030: subscribe event %d -> %d\n", evts[i],
               ar_ioctl(dev, BB_SET_EVENT_SUBSCRIBE, &ev, NULL));
    }
    // Open8030's own: TX power changes (event 14). Only there - a stock
    // radio has no such event.
    if (on_ar_libre() && radio_fw == 1) {
        bb_set_event_callback_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = (bb_event_e)14;
        ev.callback = ar_event_log_cb<14>;
        ev.user = this;
        printf("ar8030: subscribe event 14 (TX power) -> %d\n", ar_ioctl(dev, BB_SET_EVENT_SUBSCRIBE, &ev, NULL));
        // And the distance, pushed (event 15; an Open8030 without it refuses).
        ev.event = (bb_event_e)15;
        ev.callback = ar_event_log_cb<15>;
        printf("ar8030: subscribe event 15 (distance) -> %d\n", ar_ioctl(dev, BB_SET_EVENT_SUBSCRIBE, &ev, NULL));
    }
}

void Ar8030Link::configure_link() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;

    bb_mac_t macs[BB_CONFIG_MAX_SLOT_CANDIDATE];
    int n = peer_ ? 1 : native_sdio_ ? 0 : ar_load_candidate_macs(macs, BB_CONFIG_MAX_SLOT_CANDIDATE);
    if (peer_) memcpy(macs[0].addr, peer_->data(), 4);
    if (n > 0 && !native_sdio_) {
        for (int i = 0; i < n; i++)
            printf("ar8030: candidate[%d] = %02X:%02X:%02X:%02X\n", i,
                   macs[i].addr[0], macs[i].addr[1], macs[i].addr[2], macs[i].addr[3]);
        bb_set_candidate_t cand;
        memset(&cand, 0, sizeof(cand));
        cand.slot = BB_SLOT_0;
        cand.mac_num = (uint8_t)n;
        memcpy(cand.mac_tab, macs, sizeof(bb_mac_t) * n);
        printf("ar8030: BB_SET_CANDIDATES -> %d\n", ar_ioctl(dev, BB_SET_CANDIDATES, &cand, NULL));
    } else if (!n) {
        // Empty on a goggle never paired under stock (fpvOS merges stock's
        // list off the NAND at boot) and never bound here: not a fault, just
        // nothing to link with yet.
        printf("ar8030: no paired air units - bind one from the menu\n");
    }

    // Which side are we? The VRX runs as BB_ROLE_DEV (stock logs "rold=DEV"),
    // and a DEV associates by being told the AP's MAC - BB_SET_CANDIDATES is
    // the AP-side command and does nothing for us on its own.
    bb_get_status_in_t sin;
    memset(&sin, 0, sizeof(sin));
    sin.user_bmp = 0x03FF;
    bb_get_status_out_t sout;
    memset(&sout, 0, sizeof(sout));
    if (ar_ioctl(dev, BB_GET_STATUS, &sin, &sout) == 0) {
        printf("ar8030: role=%u (%s) mode=%u cfg_sbmp=0x%02x rt_sbmp=0x%02x\n",
               sout.role, sout.role == BB_ROLE_AP ? "AP" : "DEV", sout.mode,
               sout.cfg_sbmp, sout.rt_sbmp);
        if (sout.role == BB_ROLE_DEV && n > 0) {
            // Which of the paired units to name before the link: the one last
            // linked (identify_peer() saves it), not simply the oldest entry
            // of stock's pairing ring - that named an air unit that was not
            // there and kept every per-air setting under its key. The link
            // itself goes by the whole candidate list; identify_peer() fixes
            // the choice once the radio says who answered.
            int idx = (ap_index >= 0 && ap_index < n) ? ap_index : 0;
            if (ap_index == 0) {
                const unsigned last = (unsigned)settings_.get(radio::setting::ar8030_last_peer);
                for (int i = 0; i < n; i++) {
                    const unsigned m = ((unsigned)macs[i].addr[0] << 24) | ((unsigned)macs[i].addr[1] << 16) |
                                       ((unsigned)macs[i].addr[2] << 8) | macs[i].addr[3];
                    if (last && m == last) { idx = i; break; }
                }
            }
            set_sky_key_from_mac(macs[idx].addr);   // per-VTX settings namespace
            load_sky_link();                        // its TX power
            bb_set_ap_mac_t ap;
            memset(&ap, 0, sizeof(ap));
            ap.mac = macs[idx];
            printf("ar8030: BB_SET_AP_MAC(%02X:%02X:%02X:%02X) -> %d\n",
                   ap.mac.addr[0], ap.mac.addr[1], ap.mac.addr[2], ap.mac.addr[3],
                   ar_ioctl(dev, BB_SET_AP_MAC, &ap, NULL));
        }
    }

    // Pin the transmit power before anything keys the PA: left alone it sits
    // at the config default of up to 28 dBm, which browns the board out as
    // soon as we transmit. Honours the saved level including its AUTO flag.
    apply_tx_power(tx_power_mw);

    if (bandwidth < 0) {
        printf("ar8030: bandwidth left to the link (ar8030_bw unset)\n");
    } else {
        bb_set_bandwidth_t bw;
        memset(&bw, 0, sizeof(bw));
        bw.slot = BB_SLOT_0;
        bw.dir = BB_DIR_TX;
        bw.bandwidth = (uint8_t)bandwidth;
        int rc_bw = ar_ioctl(dev, BB_SET_BANDWIDTH, &bw, NULL);
        printf("ar8030: BB_SET_BANDWIDTH(%d) -> %d\n", bandwidth, rc_bw);
    }
    // Bandwidth answers -7 *here* because the link is not up yet, but once the
    // handshake has run every value 0-5 is accepted (verified twice with
    // --debug-probe-rf). So the -7 is a timing artifact, not a missing
    // capability, and the control is offered regardless.
    rf_caps |= gnd::RF_CAP_BW;

    // MCS mode and LNA are set to the values we want anyway (auto/auto), so
    // this doubles as a harmless capability check.
    bb_set_mcs_mode_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.auto_mode = 1;
    int rc_mcs = ar_ioctl(dev, BB_SET_MCS_MODE, &mm, NULL);
    printf("ar8030: BB_SET_MCS_MODE(auto=1) -> %d\n", rc_mcs);
    if (rc_mcs == 0) rf_caps |= gnd::RF_CAP_MCS;

    bb_set_lna_mode_t ln;
    memset(&ln, 0, sizeof(ln));
    ln.mode = -1;                       // -1 = automatic gain
    int rc_lna = ar_ioctl(dev, BB_SET_LNA_MODE, &ln, NULL);
    printf("ar8030: BB_SET_LNA_MODE(auto) -> %d\n", rc_lna);
    if (rc_lna == 0) rf_caps |= gnd::RF_CAP_LNA;

    // Channel adaptation: with auto_mode=1 the baseband scans/hops to find its
    // AP, which is what we want when we do not know the air unit's channel.
    // Only in manual mode does an explicit BB_SET_FREQ mean anything.
    bb_set_chan_mode_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.auto_mode = chan_auto ? 1 : 0;
    int rc_cm = ar_ioctl(dev, BB_SET_CHAN_MODE, &cm, NULL);
    printf("ar8030: BB_SET_CHAN_MODE(auto=%d) -> %d\n", cm.auto_mode, rc_cm);
    if (rc_cm == 0) rf_caps |= gnd::RF_CAP_CHAN;
    // Open8030 measures only the work list's channels (the rest read 0, which
    // the channel page drew as the busiest there is), and keeps it across a
    // kestrel-gnd restart: an earlier manual pick could have left it at one.
    if (radio_fw == 1)
        printf("ar8030: work list = every channel (the scan's list) -> %d\n", work_list_all());

    enable_ranging();

    printf("ar8030: RF capabilities: power=%d mcs=%d lna=%d bandwidth=%d chan=%d\n",
           !!(rf_caps & gnd::RF_CAP_POWER), !!(rf_caps & gnd::RF_CAP_MCS),
           !!(rf_caps & gnd::RF_CAP_LNA),   !!(rf_caps & gnd::RF_CAP_BW),
           !!(rf_caps & gnd::RF_CAP_CHAN));
    model_.update_ground([&](gnd::RadioState& r) { r.rf_caps = rf_caps; });

    if (chan_auto) {
        printf("ar8030: channel auto mode - skipping fixed BB_SET_FREQ\n");
        return;
    }

    bb_set_freq_t fr;
    memset(&fr, 0, sizeof(fr));
    fr.user = 0;
    fr.dir_bmp = (1 << BB_DIR_TX) | (1 << BB_DIR_RX);
    fr.freq_khz = freq_khz;
    printf("ar8030: BB_SET_FREQ(%u kHz) -> %d\n", freq_khz,
           ar_ioctl(dev, BB_SET_FREQ, &fr, NULL));
}

// The air unit pushes telemetry onto the port-2 socket continuously. Nothing
// consumed it after the startup handshake, so the SDK's per-socket RX ring
// (SOCK_LEN_APP_TO_DAEMON, 256 KB) filled up and every later packet was dropped
// by so_rpc_cb() with "recv data leak!! get = N , push= 0". Drain it every pass.
//
// NB: bb_socket_read() blocks forever on timeout <= 0 (it falls through to a
// bare pthread_cond_wait), so the poll below must pass a positive timeout.
void Ar8030Link::drain_control_socket() {
    if (ctrl_sockfd < 0) return;
    uint8_t buf[1024];
    for (int i = 0; i < 64; i++) {          // bounded: never starve the video read
        int n = read_queued(ctrl_sockfd, buf, sizeof(buf));
        if (n <= 0) break;                  // -1 here is just "ring empty"
        on_ctrl_bytes(buf, n);

        // Raw capture of whatever the air unit is sending, for offline
        // analysis. 4 MB is ~18 minutes at the measured 3.8 KB/s.
        if (!dump_path.empty() && dump_written < 4u * 1024 * 1024) {
            if (!dump_fp) {
                dump_fp = (void *)fopen(dump_path.c_str(), "wb");
                if (dump_fp) printf("ar8030: dumping port-2 to %s\n", dump_path.c_str());
                else dump_path.clear();
            }
            if (dump_fp) {
                fwrite(buf, 1, (size_t)n, (FILE *)dump_fp);
                fflush((FILE *)dump_fp);
                dump_written += (unsigned long long)n;
            }
        }
    }
}

void Ar8030Link::drain_msp_socket() {
    if (msp_bb_port <= 0) return;
    if (msp_bb_port == 2) return;        // teed off the control socket instead

    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;

    if (msp_sockfd < 0) {
        bb_sock_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.tx_buf_size = 0x0800;
        opt.rx_buf_size = 0x0800;
        msp_sockfd = bb_socket_open(dev, (bb_slot_e)slot, (uint32_t)msp_bb_port,
                                    BB_SOCK_FLAG_RX | BB_SOCK_FLAG_TX, &opt);
        printf("ar8030: MSP socket (port %d) fd=%d\n", msp_bb_port, msp_sockfd);
        if (msp_sockfd < 0) {
            msp_bb_port = 0;             // do not retry every pass
            return;
        }
    }

    uint8_t buf[1024];
    for (int i = 0; i < 64; i++) {       // bounded, like the telemetry drain
        int n = read_queued(msp_sockfd, buf, sizeof(buf));
        if (n <= 0) break;               // -1 is just "ring empty"

        // First bytes go to the log verbatim. If this port carries MSP at all
        // the '$' framing is visible immediately, and if it carries something
        // else that is worth seeing rather than silently feeding the parser.
        static int dumped = 0;
        if (dumped < 4) {
            dumped++;
            printf("ar8030: msp rx %d bytes:", n);
            for (int j = 0; j < n && j < 32; j++) printf(" %02X", buf[j]);
            printf("%s\n", n > 32 ? " ..." : "");
        }

        msp_bytes += (unsigned long long)n;
        fc_out_.bytes(buf, (size_t)n);
    }
}

// --debug-probe-rf: find out which RF controls this board actually honours.
// Unlike the mode probe these are all local ioctls on our own radio - they do
// not touch the air unit - so this is safe to run on a live link (it does
// briefly perturb TX power / MCS, and restores stock's settings at the end).
void Ar8030Link::probe_rf_ioctls() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    printf("ar8030: ---- RF ioctl probe ----\n");

    // Capped at stock's 24 dBm on purpose: this board browns out under load,
    // and a sweep to 33 dBm/2W is a good way to make it reset mid-probe.
    for (int dbm = 8; dbm <= 24; dbm += 1) {
        bb_set_pwr_in_t p; memset(&p, 0, sizeof(p));
        p.usr = 0; p.pwr = (uint8_t)dbm;
        printf("ar8030: rfprobe POWER %2d dBm -> %d\n", dbm,
               ar_ioctl(dev, BB_SET_POWER, &p, NULL));
    }
    for (int a = 0; a <= 1; a++) {
        bb_set_pwr_auto_in_t p; memset(&p, 0, sizeof(p));
        p.pwr_auto = (uint8_t)a;
        printf("ar8030: rfprobe POWER_AUTO %d -> %d\n", a,
               ar_ioctl(dev, BB_SET_POWER_AUTO, &p, NULL));
    }
    for (int a = 0; a <= 1; a++) {
        bb_set_mcs_mode_t p; memset(&p, 0, sizeof(p));
        p.auto_mode = (uint8_t)a;
        printf("ar8030: rfprobe MCS_MODE auto=%d -> %d\n", a,
               ar_ioctl(dev, BB_SET_MCS_MODE, &p, NULL));
    }
    for (int m = 0; m <= 13; m++) {
        bb_set_mcs_t p; memset(&p, 0, sizeof(p));
        p.slot = 0; p.mcs = (uint8_t)m;
        printf("ar8030: rfprobe MCS %2d -> %d\n", m,
               ar_ioctl(dev, BB_SET_MCS, &p, NULL));
    }
    for (int m = -1; m <= 3; m++) {
        bb_set_lna_mode_t p; memset(&p, 0, sizeof(p));
        p.mode = (int8_t)m;
        printf("ar8030: rfprobe LNA %2d -> %d\n", m,
               ar_ioctl(dev, BB_SET_LNA_MODE, &p, NULL));
    }
    for (int b = 0; b <= 5; b++) {
        bb_set_bandwidth_t p; memset(&p, 0, sizeof(p));
        p.slot = 0; p.dir = 1; p.bandwidth = (uint8_t)b;
        printf("ar8030: rfprobe BANDWIDTH %d -> %d\n", b,
               ar_ioctl(dev, BB_SET_BANDWIDTH, &p, NULL));
    }
    for (int a = 0; a <= 1; a++) {
        bb_set_chan_mode_t p; memset(&p, 0, sizeof(p));
        p.auto_mode = (uint8_t)a;
        printf("ar8030: rfprobe CHAN_MODE auto=%d -> %d\n", a,
               ar_ioctl(dev, BB_SET_CHAN_MODE, &p, NULL));
    }

    // Restore what configure_link() set up, so the probe leaves no trace.
    printf("ar8030: ---- RF probe done, restoring ----\n");
    { bb_set_pwr_auto_in_t p; memset(&p, 0, sizeof(p)); p.pwr_auto = 0;
      ar_ioctl(dev, BB_SET_POWER_AUTO, &p, NULL); }
    { bb_set_pwr_in_t p; memset(&p, 0, sizeof(p));
      p.usr = 0; p.pwr = (uint8_t)tx_power_dbm;
      ar_ioctl(dev, BB_SET_POWER, &p, NULL); }
    { bb_set_mcs_mode_t p; memset(&p, 0, sizeof(p)); p.auto_mode = 1;
      ar_ioctl(dev, BB_SET_MCS_MODE, &p, NULL); }
    { bb_set_chan_mode_t p; memset(&p, 0, sizeof(p)); p.auto_mode = chan_auto ? 1 : 0;
      ar_ioctl(dev, BB_SET_CHAN_MODE, &p, NULL); }
}

// --debug-probe-modes: walk a candidate list and report which the camera
// acknowledges. The stock GlassesUI only offers four FPV modes, but that is its
// menu, not necessarily the hardware's limit - this asks the hardware directly.
//
// Caveat: a status-0 ack means the command was accepted, not that the camera
// really delivers that mode. Confirm anything interesting by watching the
// decoder's reported geometry and frame rate afterwards.
void Ar8030Link::probe_video_modes() {
    static const struct { uint16_t w, h; uint8_t fps; } cand[] = {
        { 1280,  720,  30 }, { 1280,  720,  50 }, { 1280,  720,  60 },
        { 1280,  720,  90 }, { 1280,  720, 100 }, { 1280,  720, 120 },
        { 1920, 1080,  30 }, { 1920, 1080,  50 }, { 1920, 1080,  60 },
        { 1920, 1080,  90 }, { 1920, 1080, 100 }, { 1920, 1080, 120 },
        { 2560, 1440,  30 }, { 2560, 1440,  60 },
        { 2720, 1528,  30 }, { 2720, 1528,  60 },
        { 3840, 2160,  30 }, { 3840, 2160,  60 },
    };
    printf("ar8030: ---- mode probe (ch%d) ----\n", mode_chn);
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        std::vector<uint8_t> f = sky_proto.set_chn_res((uint8_t)mode_chn,
                                                       cand[i].w, cand[i].h, cand[i].fps);
        int w = bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 500);
        int st = (w > 0) ? wait_sky_status(sky::CMD_SET_CHN_RES, 4000) : -2;
        const char *verdict = st == 0  ? "ACCEPTED"
                            : st >  0  ? "NAK"
                            : st == -1 ? "no ack (inconclusive)" : "write failed";
        if (st > 0) printf("ar8030: probe %4ux%-4u@%-3u -> %s status=%d\n",
                           cand[i].w, cand[i].h, cand[i].fps, verdict, st);
        else        printf("ar8030: probe %4ux%-4u@%-3u -> %s\n",
                           cand[i].w, cand[i].h, cand[i].fps, verdict);
        // Let the camera finish restarting its encoder, draining video the
        // whole time so the video ring cannot back the USB path up.
        for (int t = 0; t < 3000 && !*should_stop; t += 50) { drain_video_socket(); usleep(50000); }
        if (*should_stop) { printf("ar8030: mode probe aborted (stopping)\n"); return; }
    }
    printf("ar8030: ---- probe done, restoring 1920x1080@120 ----\n");
    std::vector<uint8_t> f = sky_proto.set_chn_res((uint8_t)mode_chn, 1920, 1080, 120);
    bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 500);
    read_sky_ack(sky::CMD_SET_CHN_RES, 1200);
}

bool Ar8030Link::connect_bb() {
    // The whole bring-up is bracketed, not just bb_socket_open. Every one of
    // these SDK calls waits on the daemon, and a run caught in the act stopped
    // inside bb_dev_open - before a socket was ever asked for. Timing one call
    // only catches the parks that happen in that call; timing the sequence
    // catches all of them, and "we started bringing the link up and never
    // finished" is the condition worth acting on anyway.
    BringupGuard bringup_guard;
    bb_host_t* phost = nullptr;
    if (bb_host_connect(&phost, host.c_str(), host_port)) {
        if (on_ar_libre())
            printf("ar8030: bb_host_connect failed - is arlink.ko loaded and the radio up?\n");
        else
            printf("ar8030: bb_host_connect(%s:%d) failed - is /ar8030soc/daemon running?\n",
                   host.c_str(), host_port);
        return false;
    }
    bb_host = phost;
    static bool said_library = false;
    if (!said_library) {
        said_library = true;
        printf("ar8030: client library: %s\n",
               on_ar_libre() ? arlink_version() : "Artosyn's, through the daemon");
    }

    bb_dev_list_t* devs = nullptr;
    int dev_nr = bb_dev_getlist(phost, &devs);
    if (dev_nr <= 0 || !devs) {
        printf("ar8030: bb_dev_getlist found no baseband (ret=%d)\n", dev_nr);
        return false;
    }

    bb_dev_handle_t* hdev = bb_dev_open(devs[0]);
    bb_dev_freelist(devs);
    if (!hdev) {
        printf("ar8030: bb_dev_open failed\n");
        return false;
    }
    bb_dev = hdev;
    bb_sdk_shim_report();

    // bb_init/bb_start are idempotent from the app's point of view: the daemon
    // may already have brought the baseband up for another client.
    //
    // These are where bring-up parks, and it is worth being precise about why,
    // because the comment that used to sit here said we could not bound them
    // and that was simply wrong.
    //
    // bb_init() is bb_ioctl(BB_INIT_REQ) and bb_start() is
    // bb_ioctl(BB_START_REQ). bb_ioctl() hard-codes its timeout to -1, which
    // reaches bs_send_usbpack_and_wait() as the untimed branch of a condition
    // wait. When the daemon does not answer BB_INIT_REQ - which it sometimes
    // does not - the thread waits on that condvar for the life of the process.
    // Traced on hardware: "[bbio] req=0xB000002 tmo=-1 held" with no matching
    // return, and the wait still sitting there 52 seconds later.
    //
    // But the wrapper is the only thing that is untimed. bb_ioctl_ex() takes
    // the same requests with a caller-supplied deadline, so issuing them
    // directly bounds exactly the call that hangs, with no change to the vendor
    // library. A caller that can time out can also retry, which one inside an
    // untimed wait cannot do at all.
    auto bounded_req = [&](uint32_t req, const char* name) -> bool {
        for (int attempt = 1; attempt <= 2; attempt++) {
            int rc = bb_ioctl_ex(hdev, req, nullptr, nullptr, kBringupReqTimeoutMs);
            if (rc == 0) return true;
            // Native firmware may already be initialized. START and the role
            // check below must still succeed; never generalize this to USB.
            if (native_sdio_ && req == BB_INIT_REQ && rc == -7) return true;
            printf("ar8030: %s returned %d (attempt %d/2)\n", name, rc, attempt);
            if (*should_stop) return false;
        }
        printf("ar8030: %s never answered - the %s is not talking\n", name,
               on_ar_libre() ? "baseband" : "daemon");
        return false;
    };
    if (!bounded_req(BB_INIT_REQ, "bb_init")) return false;
    if (*should_stop) return false;
    if (native_sdio_) {
        // Set the paired AP before START. A zero AP keeps a new receiver
        // unpaired, with its menu and binding controls available.
        const uint8_t power_auto[3] = {0, 14, 14}, power[2] = {0, 14};
        if (bb_ioctl_ex(hdev, BB_SET_POWER_AUTO, power_auto, nullptr, kBringupReqTimeoutMs) != 0 ||
            bb_ioctl_ex(hdev, BB_SET_POWER, power, nullptr, kBringupReqTimeoutMs) != 0) return false;
        bb_set_ap_mac_t ap{};
        if (peer_) memcpy(ap.mac.addr, peer_->data(), 4);
        if (bb_ioctl_ex(hdev, BB_SET_AP_MAC, &ap, nullptr, kBringupReqTimeoutMs) != 0) return false;
    }
    if (!bounded_req(BB_START_REQ, "bb_start")) return false;
    if (*should_stop) return false;

    if (native_sdio_) {
        bb_get_status_in_t in{}; in.user_bmp = 0x03ff;
        bb_get_status_out_t out{};
        uint32_t got = 0;
        if (!arlink_request || arlink_request(BB_GET_STATUS, &in, sizeof(in), &out, sizeof(out), &got, kBringupReqTimeoutMs) != 0 ||
            got < offsetof(bb_get_status_out_t, link_status) + sizeof(bb_link_status_t) || out.role != BB_ROLE_DEV) {
            printf("ar8030: native ground role could not be verified\n");
            return false;
        }
    }

    // Match what the stock ar_ldy_gnd does on a clean boot (captured with the
    // daemon MITM proxy): the video socket is opened RX|TX with explicit buffer
    // sizes, not RX-only with default buffers. Opening it RX-only yields a
    // socket that never receives anything.
    //   video (port 3): flags=3, tx_buf=0x0800, rx_buf=0xDC00
    //   telemetry (port 2): flags=3, tx_buf=0x0800, rx_buf=0x0800
    bb_sock_opt_t sopt;
    memset(&sopt, 0, sizeof(sopt));
    sopt.tx_buf_size = 0x0800;
    sopt.rx_buf_size = (video_port == 3) ? 0xDC00 : 0x0800;
    subscribe_events();

    sockfd = bb_socket_open(hdev, (bb_slot_e)slot, (uint32_t)video_port,
                            BB_SOCK_FLAG_RX | BB_SOCK_FLAG_TX, &sopt);
    if (sockfd < 0) {
        printf("ar8030: bb_socket_open failed, slot=%d video_port=%d ret=%d\n",
               slot, video_port, sockfd);
        return false;
    }

    printf("ar8030: video socket open (slot=%d port=%d fd=%d)\n", slot, video_port, sockfd);
    // Arm the resync on every socket we open, not only on an RF state change.
    //
    // The transition to link state 2 is one way a stream begins and not the
    // only one: reopening after an idle timeout, or reconnecting while the
    // radio never visibly dropped, both start a fresh stream with no state
    // transition to notice. Those paths let the air unit's startup burst
    // through unfiltered - which is why coming back from IDLE_2 could still
    // race the picture and cut the acquiring screen short.
    //
    // Arming here covers every path, because there is no stream at all without
    // this socket. On a first connect it costs nothing: the same cadence test
    // decides when to open the gate either way.
    drop_pictures();
    resync_          = true;
    resync_live_     = 0;
    resync_prev_us_  = 0;
    resync_prev_cap_ = 0;
    resync_start_ms_ = now_ms();
    bb_watchdog_socket_open();
    if (*should_stop) return false;
    configure_link();
    if (*should_stop) return false;
    // --ar8030-pair binds once at startup. It parks a request like every other
    // caller rather than running its own loop, so there is a single bind
    // implementation and the on-screen indicator works for it too. Cleared so
    // a reconnect does not silently re-open the pairing window.
    if (do_pair) { do_pair = false; request_bind(); }
    // Stock's link-setup SETs, then its PRJ_DISPATCH. Order follows the
    // bbproxy capture: the MCS table lands after the link is up.
    replay_stock_rf_setup();

    // Re-apply the user's TX power. The replay is stock's startup sequence
    // verbatim, and stock's own power lives in it: SET 8 is BB_SET_POWER with
    // {usr 0, 0x18} = 24 dBm, and the two SET 9s set the auto range to 27/24.
    // So the replay silently stomps whatever configure_link() just set - pick
    // 100mW in the menu and the radio ends up at 500mW, with the OSD correctly
    // reporting 500mW while the menu says 100mW.
    apply_tx_power(tx_power_mw);

    // Stock sends this immediately after its socket setup and before anything
    // else - see send_prj_rf_config(). Ordering copied from ar_ldy_gnd 0x9bca0.
    send_prj_rf_config();

    // The air-side half. Section 31 established the same shape for the RF
    // channel: both ends have to be told, or the link just desynchronises.
    if (air_bw >= 0) request_setting(CAM_BW, air_bw);
    // kestrel-air's caps (Max Bitrate, Max Bandwidth) go when it announces
    // itself (note_air_announce): the stock air app does not know them.

    if (skip_handshake) printf("ar8030: handshake SKIPPED (--debug-no-handshake)\n");
    else send_air_handshake(true);

    // Bandwidth, again. The call in configure_link() answers -7 because the
    // link is not up yet; post-handshake every gear 0-5 is accepted (verified
    // with --debug-probe-rf). Skipping this left the radio on its 5 MHz
    // power-on default while stock runs 20 MHz - a 4x narrower channel, and
    // the air unit sizes its encoder to the link, which is where the ~5x
    // bitrate gap against stock came from.
    if (bandwidth >= 0) apply_bandwidth();

    // After the handshake, not before: the handshake's SET_CONFIG replay would
    // otherwise overwrite whatever we just asked for.
    if (standby_mode >= 0) request_setting(CAM_STANDBY, standby_mode);

    // Power was applied to our own radio in configure_link(), before the
    // control socket existed - push it to the air unit now that it does.
    request_setting(CAM_PWR, sky_pwr_mw(kArPwrLevels[ar_pwr_index(tx_power_mw)]));
    link_reported = false;
    start_stats();
    return true;
}

void Ar8030Link::disconnect_bb() {
    stop_stats();                        // before the handles it uses go
    drop_pictures();
    // --debug-bb-dump's file was opened lazily and never closed; on a forced exit the
    // tail of the capture was whatever had not been flushed.
    if (dump_fp) { fclose((FILE *)dump_fp); dump_fp = nullptr; }
    if (video_dump_fp) { fclose((FILE *)video_dump_fp); video_dump_fp = nullptr; }
    // Hand the channel back to AUTO/ACS before letting go of the device.
    //
    // Defensive, not a known fix: leaving the radio pinned is measurably
    // survivable (a clean exit from manual mode restarts fine, and
    // configure_link() re-asserts auto at startup anyway). But a forced exit
    // skips this path entirely, and that IS the state that wedges the next
    // bring-up - it hangs in bb_socket_open() for the video port. Restoring
    // here costs one ioctl and narrows the window.
    if (bb_dev && !chan_auto) {
        bb_set_chan_mode_t m;
        memset(&m, 0, sizeof(m));
        m.auto_mode = 1;
        printf("ar8030: restoring channel AUTO/ACS before shutdown -> %d\n",
               ar_ioctl((bb_dev_handle_t*)bb_dev, BB_SET_CHAN_MODE, &m, NULL));
        chan_auto = true;
    }
    // Instrumented: the shutdown hangs somewhere in here and the watchdog then
    // force-exits, which leaves the daemon's slot owned by a dead process and
    // wedges the next bring-up. Print around each step so the last line printed
    // names the culprit. so_deinit() in the SDK waits on exitcv with NO timeout
    // until every in-flight bb_socket_read has deregistered, so a reader that
    // never returns parks the whole teardown.
    #define SHUT_STEP(msg) do { printf("ar8030: shutdown: " msg "\n"); fflush(stdout); } while (0)
    SHUT_STEP("closing video socket");
    if (sockfd >= 0) { bb_socket_close(sockfd); sockfd = -1; }
    SHUT_STEP("closing control socket");
    if (ctrl_sockfd >= 0) { bb_socket_close(ctrl_sockfd); ctrl_sockfd = -1; }
    // --debug-msp-bb-port's: opened again at the next bring-up (drain_msp_socket).
    if (msp_sockfd >= 0) { SHUT_STEP("closing MSP socket"); bb_socket_close(msp_sockfd); msp_sockfd = -1; }
    sky_ack_ = SkyAckWait();             // its ack will not come on this socket
    // The work channel lists queued with those settings go with them (in the
    // same order); the ones of settings parked since stay. Kept, AUTO HOP's
    // next list went out as one of the link that ended.
    for (const auto& q : settings_q_)
        if (q.first == CAM_WORK_LIST && !region_q_lists_.empty()) region_q_lists_.pop_front();
    settings_q_.clear();
    // Hand the device back before letting go of the handle.
    //
    // This is the other half of bb_init/bb_start, and its absence is what
    // poisons the daemon. Closing our handle does not tell it the device is
    // free: it stays marked initialised and started, owned by a client that is
    // no longer there, and the next BB_INIT_REQ then goes unanswered for good.
    // That is why a plain reconnect - "link idle too long, reopening socket" -
    // could park the next bring-up, with nothing having crashed at all.
    //
    // Bounded like the bring-up requests, and errors are ignored: a teardown
    // that hangs or fails is not a reason to keep the handle, and past this
    // point there is nothing left to salvage anyway.
    SHUT_STEP("stopping and deinitialising");
    if (bb_dev) {
        bb_dev_handle_t *d = (bb_dev_handle_t *)bb_dev;
        bb_ioctl_ex(d, BB_STOP_REQ,   nullptr, nullptr, kBringupReqTimeoutMs);
        bb_ioctl_ex(d, BB_DEINIT_REQ, nullptr, nullptr, kBringupReqTimeoutMs);
    }
    SHUT_STEP("closing device");
    if (bb_dev)  { bb_dev_close((bb_dev_handle_t*)bb_dev); bb_dev = nullptr; }
    SHUT_STEP("disconnecting host");
    if (bb_host) { bb_host_disconnect((bb_host_t*)bb_host); bb_host = nullptr; }
    SHUT_STEP("done");
    #undef SHUT_STEP
}

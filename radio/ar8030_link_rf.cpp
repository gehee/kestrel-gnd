// The AR8030 link's RF: the channel (AUTO, AUTO HOP, the region's rules), TX
// power, bandwidth and frame, ranging, the 20/40 MHz switch.
#include "radio/ar8030_link_detail.hpp"

using namespace radio;


bool Ar8030Link::region_usable(int mhz) {
    const int cc = zones::selected_country();
    if (cc < 0) return false;
    const int max_bw = max_bw_mhz.load();
    const int cap = max_bw == 0 || max_bw >= 40 ? 40 : max_bw;
    const int bw = std::min(cap, std::max(20, link_bw_mhz.load()));
    return zones::usable(cc, mhz, bw, zones::licensed());
}

// Flight-controller MSP passthrough. Stock never reads this off a BB socket -
// its ar_ldy_gnd takes the FC OSD as UDP datagrams on :55068 and nothing in the
// stock rootfs sends to that port, so the bytes reach it as IP over a baseband
// network device we do not bring up. Reading the passthrough port directly is
// the equivalent path for a client that already owns the BB session.
//
// Opt-in via --debug-msp-bb-port: if the air unit does not serve the port,
// bb_socket_open() blocks with no timeout (section 33) and wedges bring-up.
// BB_SET_PRJ_DISPATCH (SET 200), project command 138.
//
// Recovered from ar_ldy_gnd, not guessed. The builder is at 0x9f5dc:
//
//     buf = 256 zero bytes
//     buf[0] = 0x8A                 // cmd 138
//     p = buf + 4
//     p[0] = (uint8_t)arg1          // -> buf[4]
//     *(uint32_t *)(p + 4) = arg2   // -> buf[8]
//     ar_ioctl(dev, 0x020000C8, buf, NULL, 1000)
//
// and its single caller at 0x9bca0 passes (4, 500) - guarded on the product
// subtype being 2, which is this board ("vrx pro2"):
//
//     bl  0x94d44          ; getter for the --subtype global
//     cmp w0, #2
//     b.ne skip
//     mov w1, #0x1f4       ; 500
//     mov w0, #0x4         ; 4
//     bl  0x9f5dc
//
// Confirmed on the wire: a usbmon capture of stock's link bring-up shows
// SET 200 with payload 8A 00 00 00 | 04 00 00 00 | F4 01 00 00, exactly this.
//
// The 4 and 500 are almost certainly a bandwidth gear (BB_BW_20M) and a
// transmit power in mW - stock's menu reads 20 MHz and 500 mW - but that
// reading is inferred. What is certain is the byte sequence, so we replay it
// rather than trying to be clever about the semantics.
// Stock's startup SET sequence, replayed byte-for-byte.
//
// Taken from tools/bbproxy-cleanboot.log (payload = frame[18 .. 18+len]). Every
// one of these is in the extracted v2 ioctl table with exactly the length seen
// on the wire, which is a good consistency check on the decode:
//
//   SET 5 =1B  SET 8 =2B  SET 9 =3B  SET 12=2B
//   SET 13=2B  SET 16=1B  SET 32=2B  SET 36=16B
//
// The SET 36 entries are the MCS adaptation policy - ar_ldy_gnd logs them as
//   "[mcs item],idx=%d,ldpc_dw_conti_num=%d,snr=[%d,%d],ldpc=[%d,%d],keep=[%d,%d]"
// and the builder at 0xa0754 lays the 16 bytes out as
//   [0..3] rsv  [4] mcs  [5] ldpc_dw_conti_num  [6..7] snr_up  [8..9] snr_dw
//   [10] ldpc_up  [11] ldpc_dw  [12..13] keep_up  [14..15] keep_dw
// Bytes 1..3 read as 9D D9 85 in the capture only because that builder never
// writes them - it is uninitialised stack, so we send zeroes.
//
// The seven entries cover MCS {1,2,5,7,8,10,12}, exactly the levels section 24
// found this radio accepts, with strictly increasing SNR thresholds. We send
// none of this today, which is the likeliest reason our link sits at 2.5 MHz /
// MCS 8 with a 3.23 Mbps ceiling while stock reaches 20 MHz and ~18.7.
struct StockSet { int order; int len; uint8_t data[16]; };
static const StockSet kStockRfSetup[] = {
    {  9, 3, { 0x00, 0x1B, 0x18 } },
    {  9, 3, { 0x02, 0x18, 0x18 } },
    {  8, 2, { 0x00, 0x18 } },
    { 12, 2, { 0x08, 0x00 } },
    { 13, 2, { 0x08, 0x02 } },
    { 32, 2, { 0x00, 0x01 } },
    { 12, 2, { 0x08, 0x01 } },
    { 16, 1, { 0x02 } },
    {  5, 1, { 0x01 } },
    { 36, 16, { 0,0,0,0, 0x01, 0x02, 0x24,0x00, 0x1D,0x00, 0x00, 0x04, 0xE8,0x03, 0x0F,0x00 } },
    { 36, 16, { 0,0,0,0, 0x02, 0x02, 0x5C,0x00, 0x41,0x00, 0x00, 0x04, 0xDC,0x05, 0x0F,0x00 } },
    { 36, 16, { 0,0,0,0, 0x05, 0x02, 0xA8,0x00, 0x77,0x00, 0x00, 0x04, 0xDC,0x05, 0x0F,0x00 } },
    { 36, 16, { 0,0,0,0, 0x07, 0x02, 0x2F,0x01, 0xF1,0x00, 0x00, 0x04, 0x20,0x03, 0x0F,0x00 } },
    { 36, 16, { 0,0,0,0, 0x08, 0x02, 0x56,0x02, 0xDB,0x01, 0x00, 0x03, 0x20,0x03, 0x0F,0x00 } },
    { 36, 16, { 0,0,0,0, 0x0A, 0x02, 0xA8,0x04, 0xB3,0x03, 0x00, 0x04, 0xE8,0x03, 0x0C,0x00 } },
    { 36, 16, { 0,0,0,0, 0x0C, 0x02, 0x36,0x07, 0xBA,0x05, 0x00, 0x02, 0xE8,0x03, 0x01,0x00 } },
};

void Ar8030Link::replay_stock_rf_setup() {
    if (!replay_stock_rf) { printf("ar8030: stock RF setup replay disabled\n"); return; }
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;

    int ok = 0, fail = 0;
    for (size_t i = 0; i < sizeof(kStockRfSetup) / sizeof(kStockRfSetup[0]); i++) {
        const StockSet &s = kStockRfSetup[i];
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, s.data, (size_t)s.len);
        if (s.order == 16) buf[0] = (uint8_t)frame_mode;   // RADIO > Radio Frame (set_frame_mode())
        int req = (2 << 24) | s.order;          // BB_REQUEST(BB_REQ_SET, order)
        int rc = ar_ioctl(dev, req, buf, NULL);
        if (rc == 0) ok++; else fail++;
        printf("ar8030: stock SET %-3d ->%3d  [", s.order, rc);
        for (int j = 0; j < s.len; j++) printf(" %02X", buf[j]);
        printf(" ]\n");
    }
    printf("ar8030: stock RF setup replay: %d accepted, %d refused\n", ok, fail);
}

// The radio's TDD frame. SET order 16 = 2 (in the stock setup above) is the
// firmware's "frame-change mode 2": the uplink cut to 6 symbols at MCS 0 and
// the slot's 400 us guard gone - 4650.88 us at 20 MHz against mode 0's
// 5633.92, and 3250.56 at 40 MHz against 4233.60. Every link that forms starts on mode 0 - a new
// one, or the same one re-formed by a step to or from 40 MHz - so it is sent
// again after each link-up: from the link event, as a re-form is often over
// before the video loop looks (seen 2026-10-14: the step to 40 MHz stayed on
// mode 0), 1 s on and 1 s clear of the radio's own bandwidth steps, which
// come right at link-up (one sent with them re-formed the link once).
// Mode 2 was the fastest at both widths and carries
// more (2026-10-14, 864p120, MCS 10: capture->lit 23.5 vs 23.8 ms and 31.4 vs
// 25.9 Mbps at 20 MHz, 21.8 vs 22.1-22.9 ms and 44.9 vs 34.5 Mbps at 40).
// Its uplink is shorter, though (6 symbols at MCS 0 for 12 at MCS -1, ~3 dB
// less margin by the firmware's numbers, untested at range): RADIO > Radio
// Frame STANDARD asks for mode 0 instead, here and in the startup setup,
// for testing it in the field.
void Ar8030Link::set_frame_mode() {
    if (!replay_stock_rf) return;
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    uint8_t buf[16] = { (uint8_t)frame_mode };
    printf("ar8030: radio frame mode %d (%s) -> %d\n", frame_mode.load(), frame_mode == 2 ? "short" : "standard",
           ar_ioctl(dev, (2 << 24) | 16, buf, NULL));
    frame_clock_.reframe();                   // another frame, if it was not on it
}

void Ar8030Link::send_prj_rf_config() {
    if (prj_rf_bw < 0 || prj_rf_pwr_mw < 0) {
        printf("ar8030: PRJ_DISPATCH rf config disabled\n");
        return;
    }
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x8A;                                  // cmd 138
    buf[4] = (uint8_t)prj_rf_bw;
    uint32_t pwr = (uint32_t)prj_rf_pwr_mw;
    memcpy(buf + 8, &pwr, sizeof(pwr));             // little-endian, as captured

    int rc = ar_ioctl(dev, BB_SET_PRJ_DISPATCH, buf, NULL);
    printf("ar8030: BB_SET_PRJ_DISPATCH cmd138(%d, %d mW) -> %d\n",
           prj_rf_bw, prj_rf_pwr_mw, rc);
}

// Both power ioctls, in one place. Startup and the menu row have to agree:
// hardcoding auto=off at startup meant a saved "500mW AUTO" came back as a
// fixed level after a reboot.
//
// Auto goes first. The other order lets the adaptation loop move off the level
// we just asked for before it has even been read back.
void Ar8030Link::apply_tx_power(int mw) {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    const ar_pwr_level &lv = kArPwrLevels[ar_pwr_index(mw)];

    // 3 bytes, not 1. Our bb_get_pwr_auto_out_t is a single uint8_t, but the
    // v2 table (extracted from stock) says SET 9 takes 3, so bb_ioctl copied
    // two bytes of stack past the struct. Stock's own payloads read as
    // {enable, max_dBm, min_dBm} - 00 1B 18 = off/27/24, 01 1B 0E = on/27/14 -
    // which matches its "en power auto [%d %d]" log.
    uint8_t pa[3] = { (uint8_t)(lv.automode ? 1 : 0),
                      (uint8_t)lv.dbm,
                      (uint8_t)(lv.automode ? kArPwrLevels[0].dbm : lv.dbm) };
    int rc_auto = ar_ioctl(dev, BB_SET_POWER_AUTO, pa, NULL);

    bb_set_pwr_in_t p;
    memset(&p, 0, sizeof(p));
    p.usr = 0;
    p.pwr = (uint8_t)lv.dbm;
    int rc_pwr = ar_ioctl(dev, BB_SET_POWER, &p, NULL);

    printf("ar8030: TX power %s (%d dBm, auto=%d) -> auto:%d set:%d\n",
           lv.label, lv.dbm, lv.automode ? 1 : 0, rc_auto, rc_pwr);
    tx_power_dbm  = lv.dbm;
    tx_power_mw   = lv.mw;
    tx_power_auto = lv.automode;
    if (rc_pwr == 0) rf_caps |= gnd::RF_CAP_POWER;

    // Mirror it to the air unit. Guarded because configure_link() calls this
    // before the control socket exists; the post-handshake call covers startup.
    if (ctrl_sockfd >= 0) request_setting(CAM_PWR, sky_pwr_mw(lv));
}

void Ar8030Link::apply_bandwidth() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    bb_set_bandwidth_t p;
    memset(&p, 0, sizeof(p));
    p.slot = BB_SLOT_0;
    p.dir = BB_DIR_TX;
    p.bandwidth = (uint8_t)bandwidth;
    int rc = ar_ioctl(dev, BB_SET_BANDWIDTH, &p, NULL);
    static const char *bw_name[] = { "1.25M", "2.5M", "5M", "10M", "20M", "40M" };
    printf("ar8030: BB_SET_BANDWIDTH(%d = %s) post-link -> %d\n", bandwidth,
           (bandwidth >= 0 && bandwidth < 6) ? bw_name[bandwidth] : "?", rc);
}

// Apply RF changes parked by the menu. These are ioctls on our own radio, so
// they take effect immediately; the return code is logged so an unsupported
// value is visible rather than silently ignored.
void Ar8030Link::apply_pending_rf() {
    std::vector<std::pair<int,int>> todo;
    {
        std::lock_guard<std::mutex> lk(pending_mtx);
        if (pending_rf.empty()) return;
        todo.swap(pending_rf);
    }
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    for (size_t i = 0; i < todo.size(); i++) {
        const int field = todo[i].first, val = todo[i].second;
        switch (field) {
            case RF_TX_POWER: {
                // val is mW in stock's encoding (N, or N+1 for auto capped
                // at N); apply_tx_power() owns the mapping and both ioctls.
                // The menu's choice: kept, and run fitted to this air unit.
                tx_power_want_mw = val;
                apply_tx_power(ar_pwr_fit(air_prj.load(), val));
                save_sky_link();
                break;
            }
            case RF_TX_POWER_FIT:
                // The choice fitted to the air unit that just said what it is;
                // the choice stays as it was.
                apply_tx_power(ar_pwr_fit(air_prj.load(), tx_power_want_mw));
                break;
            case RF_MCS: {
                // val < 0 means AUTO: flip the mode rather than pin an index.
                bb_set_mcs_mode_t m; memset(&m, 0, sizeof(m));
                m.auto_mode = (val < 0) ? 1 : 0;
                printf("ar8030: RF MCS mode auto=%d -> %d\n", m.auto_mode,
                       ar_ioctl(dev, BB_SET_MCS_MODE, &m, NULL));
                if (val >= 0) {
                    bb_set_mcs_t p; memset(&p, 0, sizeof(p));
                    p.slot = (uint8_t)slot; p.mcs = (uint8_t)val;
                    printf("ar8030: RF MCS %d -> %d\n", val,
                           ar_ioctl(dev, BB_SET_MCS, &p, NULL));
                }
                break;
            }
            case RF_LNA: {
                bb_set_lna_mode_t p; memset(&p, 0, sizeof(p));
                p.mode = (int8_t)val;
                printf("ar8030: RF LNA mode %d -> %d\n", val,
                       ar_ioctl(dev, BB_SET_LNA_MODE, &p, NULL));
                break;
            }
            case RF_BW: {
                bb_set_bandwidth_t p; memset(&p, 0, sizeof(p));
                p.slot = (uint8_t)slot; p.dir = BB_DIR_TX; p.bandwidth = (uint8_t)val;
                printf("ar8030: RF bandwidth %d -> %d\n", val,
                       ar_ioctl(dev, BB_SET_BANDWIDTH, &p, NULL));
                break;
            }
            case RF_HOP: {
                if (val) {
                    printf("ar8030: RF channel hop ON\n");
                    search_all_channels();          // and undo any pin's work list
                    break;
                }
                bb_set_chan_mode_t m; memset(&m, 0, sizeof(m));
                m.auto_mode = 0;
                printf("ar8030: RF channel hop OFF -> %d\n",
                       ar_ioctl(dev, BB_SET_CHAN_MODE, &m, NULL));
                chan_auto = false;
                break;
            }
            case RF_CHAN: {
                // val < 0 = hand the channel back to AUTO/ACS; otherwise val is
                // the target frequency in kHz and both ends have to move.
                // BB_SET_FREQ is deliberately not used here - stock never calls
                // it, and on its own it moves nothing (section 31).
                if (val < 0) {
                    printf("ar8030: RF chan mode AUTO\n");
                    search_all_channels();
                } else {
                    int rc = set_rf_channel((uint32_t)val, false);
                    printf("ar8030: RF channel -> %u kHz rc=%d\n",
                           (unsigned)val, rc);
                    // set_rf_channel() owns chan_auto - it is the only thing
                    // that knows whether the retune landed.
                }
                break;
            }
            default: break;
        }
    }
    report_link_status();
}

// The video bitrate and the link MCS have to be read together to tell a
// link-limited stream from an encoder that is simply targeting a low rate.
// report_link_status() only fires when the link is idle, which is exactly when
// there is no bitrate to compare against - hence this.
// The baseband can measure air<->ground distance by time of flight. It is off
// by default; BB_CFG_DISTC turns it on. `offset` is a close-range calibration
// (the raw td_out reading with the units side by side) subtracted from every
// result - left at 0 here because we have not calibrated this pair, so treat
// the absolute value as uncalibrated until someone measures it at a known range.
void Ar8030Link::enable_ranging() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    bb_conf_distc_t d;
    memset(&d, 0, sizeof(d));
    d.enable  = 1;
    d.window  = 3;    // 2^3 = 8-sample average; the raw reading is noisy
    d.timeout = 9;    // 2.6*(9+1) = 26 ms
    d.offset  = 0;    // uncalibrated
    int rc = ar_ioctl(dev, BB_CFG_DISTC, &d, NULL);
    // Note: this answers -2 on this board, but ranging is NOT unavailable -
    // bb_config_gnd_pro.json already carries "dist_calc": {enable:true,
    // window:3, timeout:63, offset:20}, applied when the firmware is uploaded.
    // BB_CFG_DISTC is a runtime re-configuration and the firmware refuses it;
    // the results are readable regardless, so never gate the read on this.
    printf("ar8030: BB_CFG_DISTC -> %d%s\n", rc,
           rc ? "  (already set from bb_config dist_calc; reading anyway)" : "");
}

// -1 while the baseband has no fix.
int Ar8030Link::current_distance() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return -1;
    bb_get_distc_result_in_t in;
    memset(&in, 0, sizeof(in));
    in.slot_bmp = (uint8_t)(1 << slot);
    bb_get_distc_result_out_t out;
    memset(&out, 0, sizeof(out));
    if (ar_ioctl(dev, BB_GET_DISTC_RESULT, &in, &out) != 0) return -1;
    return out.distance[slot];
}



uint32_t Ar8030Link::link_freq_khz() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return 0;
    bb_get_status_in_t sin;
    memset(&sin, 0, sizeof(sin));
    sin.user_bmp = 0x03FF;
    bb_get_status_out_t sout;
    memset(&sout, 0, sizeof(sout));
    if (ar_ioctl(dev, BB_GET_STATUS, &sin, &sout) != 0) return 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < BB_DATA_USER_MAX; i++) {
            const bb_user_status_t &us = sout.user_status[i];
            if (us.tx_status.freq_khz == 0 && us.rx_status.freq_khz == 0) continue;
            if (pass == 0 && !(sout.rt_sbmp & (1 << i))) continue;
            return us.tx_status.freq_khz;
        }
    return 0;
}


int Ar8030Link::chan_index_for_freq(uint32_t freq_khz, uint32_t *freqs, int *n_out, int32_t *power) {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return -1;
    // Oversized for the same reason as publish_chan_scan(): the daemon writes
    // 1028 bytes, not the 260 our header's struct declares.
    uint8_t raw[4096];
    memset(raw, 0, sizeof(raw));
    if (ar_ioctl(dev, BB_GET_CHAN_INFO, NULL, raw) != 0) return -1;
    struct chan_info_real {
        uint8_t  chan_num, auto_mode, acs_chan, work_chan;
        uint32_t freq[128];
        int32_t  power[128];
    };
    const chan_info_real &ci = *(const chan_info_real *)raw;
    int n = ci.chan_num;
    if (n <= 0) return -1;
    if (n > 128) n = 128;
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (freqs) freqs[i] = ci.freq[i];
        if (power) power[i] = ci.power[i];
        if (ci.freq[i] == freq_khz) idx = i;
    }
    if (n_out) *n_out = n;
    return idx;
}

int Ar8030Link::set_rf_channel(uint32_t freq_khz, bool hop_en) {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return -1;
    if (ctrl_sockfd < 0) {
        printf("ar8030: set_rf_channel: no control socket\n");
        return -4;
    }

    uint32_t freqs[128];
    int n = 0;
    int idx = chan_index_for_freq(freq_khz, freqs, &n);
    if (idx < 0) {
        printf("ar8030: set_rf_channel(%u kHz): not in the radio's channel table\n",
               freq_khz);
        return -1;
    }
    chan_settle_ms_ = now_ms() + 6000;   // the scan's levels are the move's for a while
    printf("ar8030: set_rf_channel freq=%u kHz idx=%d hop_en=%d (of %d channels)\n",
           freq_khz, idx, (int)hop_en, n);

    // --- air unit -----------------------------------------------------------
    // Both of these are required. 0x29 declares which channels the air unit is
    // allowed to use; without it the air ACKs 0x21 with status 0 and stays put,
    // because the target is not in its permitted set. Verified on hardware.
    std::vector<uint8_t> wf = sky_proto.set_work_chan_list(&freq_khz, 1);
    int ww = bb_socket_write(ctrl_sockfd, wf.data(), (uint32_t)wf.size(), 500);
    int wst = wait_sky_status(sky::CMD_SET_WORK_CHAN_LIST, 1500);
    printf("ar8030:   sky 0x29 SET_WORK_CHAN_LIST -> %d bytes, ack=%d\n", ww, wst);
    if (ww != (int)wf.size()) return -4;

    std::vector<uint8_t> f = sky_proto.set_bb_freq(hop_en, (uint8_t)slot, freq_khz);
    int w = bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 500);
    int st = wait_sky_status(sky::CMD_SET_BB_FREQ, 1500);
    printf("ar8030:   sky 0x21 SET_BB_FREQ %u kHz -> %d bytes, ack=%d\n",
           freq_khz, w, st);
    if (w != (int)f.size()) return -4;

    // --- ground -------------------------------------------------------------
    // The ground will follow on its own while it is in ACS/auto, but that is a
    // re-acquire, not a command. Drive it explicitly so both ends are
    // deterministic, then fall back to auto if the explicit path does not take.
    if (!hop_en) {
        bb_set_chan_mode_t cm;
        memset(&cm, 0, sizeof(cm));
        cm.auto_mode = 0;
        int rc = ar_ioctl(dev, BB_SET_CHAN_MODE, &cm, NULL);
        printf("ar8030:   BB_SET_CHAN_MODE(auto=0) -> %d\n", rc);
        chan_auto = false;
        // The air unit's own hopping went off with the 0x21 above.
        region_hop_told_ = false;
        region_hop_n = 0;

        usleep(150000);

        bb_work_chan_list_t wl;
        memset(&wl, 0, sizeof(wl));
        wl.chan_num    = 1;
        wl.chan_idx[0] = (uint8_t)idx;
        rc = ar_ioctl(dev, BB_SET_WORK_CHAN_LIST, &wl, NULL);
        printf("ar8030:   BB_SET_WORK_CHAN_LIST(idx=%d) -> %d\n", idx, rc);

        usleep(150000);

        // chan_index is an index into the radio's full channel table (the one
        // BB_GET_CHAN_INFO returns), not into the work list installed above -
        // confirmed on hardware.
        bb_set_chan_t sc;
        memset(&sc, 0, sizeof(sc));
        sc.chan_dir   = BB_DIR_RX;
        sc.chan_index = (uint8_t)idx;
        rc = ar_ioctl(dev, BB_SET_CHAN, &sc, NULL);
        printf("ar8030:   BB_SET_CHAN(dir=RX idx=%d) -> %d\n", idx, rc);

    } else {
        bb_set_chan_mode_t cm;
        memset(&cm, 0, sizeof(cm));
        cm.auto_mode = 1;
        printf("ar8030:   BB_SET_CHAN_MODE(auto=1) -> %d\n",
               ar_ioctl(dev, BB_SET_CHAN_MODE, &cm, NULL));
        chan_auto = true;
    }

    // --- verify -------------------------------------------------------------
    // Poll rather than assume: the air moves first and the ground needs a
    // moment either way.
    for (int i = 0; i < 20; i++) {
        usleep(200000);
        uint32_t cur = link_freq_khz();
        if (cur == freq_khz) {
            printf("ar8030:   ground confirmed on %u kHz after %d ms\n",
                   cur, (i + 1) * 200);
            return 0;
        }
    }
    // Explicit control did not land. ACS-follow is known to work, so restore
    // auto rather than leaving the ground pinned to a channel it is not on -
    // that combination is what wedged the baseband during bring-up testing.
    printf("ar8030:   WARNING ground did not reach %u kHz (now %u kHz); "
           "restoring auto/ACS\n", freq_khz, link_freq_khz());
    search_all_channels();
    return -5;
}

// A pin narrows the ground's work channel list to the one channel
// (set_rf_channel), and channel adaptation only searches what is on that list:
// turning auto back on alone left the ground looking at a single channel, and
// an air unit that had moved - after its own restart, say - was not found
// until the idle reconnect reopened the radio, about 100 s later.
int Ar8030Link::search_all_channels() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return -1;
    uint32_t freqs[128];
    int n = 0;
    chan_index_for_freq(0, freqs, &n);          // for the channel count only
    int rc_wl = -1;
    if (n > 0) {
        bb_work_chan_list_t wl;
        memset(&wl, 0, sizeof(wl));
        wl.chan_num = (uint8_t)n;
        for (int i = 0; i < n; i++) wl.chan_idx[i] = (uint8_t)i;
        rc_wl = ar_ioctl(dev, BB_SET_WORK_CHAN_LIST, &wl, NULL);
    }
    bb_set_chan_mode_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.auto_mode = 1;
    int rc = ar_ioctl(dev, BB_SET_CHAN_MODE, &cm, NULL);
    printf("ar8030:   searching all %d channels: BB_SET_WORK_CHAN_LIST -> %d, "
           "BB_SET_CHAN_MODE(auto=1) -> %d\n", n, rc_wl, rc);
    if (rc == 0) chan_auto = true;
    region_due = true;   // AUTO: the air unit hops too, where a country is known
    return rc;
}

// AUTO's choice for a link on cur: the quietest allowed channel by our radio's
// scan when cur is not allowed (any time: the list changed), and - only at a
// pick (link-up, AUTO chosen) - when that one is at least kQuieterDb quieter
// than cur was while the link was elsewhere (free_level_; not known, no such
// move). Not twice within 30 s: a move re-forms the link. 0: stay.
uint32_t Ar8030Link::region_auto_target(uint32_t cur, const uint32_t *freqs, const int32_t *power, int n,
                                          const std::vector<uint32_t> &want, bool pick) {
    static constexpr int kQuieterDb = 6;
    uint32_t target = 0;
    int32_t level = 0, own = 0;
    for (int i = 0; i < n; i++) {
        if (freqs[i] == cur) { own = i < 128 ? free_level_[i].load() : 0; continue; }
        if (std::find(want.begin(), want.end(), freqs[i]) != want.end() && (!target || power[i] < level)) {
            target = freqs[i];
            level = power[i];
        }
    }
    if (!target) return 0;   // cur is the only allowed channel
    const int cc = zones::selected_country();
    const char *code = cc >= 0 ? zones::country(cc).code : "--";
    if (std::find(want.begin(), want.end(), cur) == want.end()) {
        printf("ar8030: AUTO: %u kHz is not allowed (%s) - moving the link to %u kHz\n", cur, code, target);
        return target;
    }
    const uint64_t now = now_ms();
    if (pick && own != 0 && own - level >= kQuieterDb && (!region_move_ms_ || now - region_move_ms_ >= 30000)) {
        printf("ar8030: AUTO: %u kHz is the quietest allowed (%d dBm, here %d without our link) - moving the link there\n",
               target, (int)level, (int)own);
        return target;
    }
    return 0;
}

std::vector<uint32_t> Ar8030Link::region_list(const uint32_t *freqs, int n) {
    std::vector<uint32_t> v;
    for (int i = 0; i < n && v.size() < 64; i++)   // kestrel-air takes 64
        if (region_usable((int)(freqs[i] / 1000))) v.push_back(freqs[i]);
    return v;
}

void Ar8030Link::region_auto_apply() {
    if (!bb_dev || ctrl_sockfd < 0 || chan_manual_cli) return;
    // Sent at every link-up (note_air_announce): an air unit that restarted
    // forgot it. Not before it has said what it is, so it does not crowd the
    // handshake; without a link the next link-up brings it.
    if (link_state_.load() != 2 || !air_ver_seen_) return;
    uint32_t freqs[128];
    int32_t power[128];
    int n = 0;
    chan_index_for_freq(0, freqs, &n, power);
    if (n <= 0) return;
    std::vector<uint32_t> want;
    if (chan_auto) want = region_list(freqs, n);
    const int cc = zones::selected_country();
    const char* code = cc >= 0 ? zones::country(cc).code : "--";
    if (want.empty()) {
        // No country, or nothing allowed: AUTO is not offered and the air unit
        // stays where it is. One we had hopping stops there, back on every
        // channel. (A pinned channel already stopped it: chan_auto is off then.)
        region_pick = false;
        if (region_hop_told_ && chan_auto) {
            const uint32_t cur = link_freq_khz();
            if (!cur) return;
            printf("ar8030: AUTO HOP: no channel allowed (%s) - the air unit stops hopping, on %u kHz\n", code, cur);
            region_q_lists_.emplace_back(freqs, freqs + std::min(n, 64));
            request_setting(CAM_WORK_LIST, std::min(n, 64));
            request_setting(CAM_HOP_OFF, (int)cur);
        }
        region_hop_told_ = false;
        region_hop_n = 0;
        return;
    }
    uint32_t cur = link_freq_khz();
    if (!cur) return;
    // AUTO HOP moves nothing itself: the air unit steers, from wherever it is.
    // Nor right after a move (the handshake's): the link may still be on its way.
    const bool pick = region_pick.exchange(false);
    const uint64_t now = now_ms();
    const uint32_t target = chan_hop.load() || (region_move_ms_ && now - region_move_ms_ < 10000) ? 0 :
                            region_auto_target(cur, freqs, power, n, want, pick);
    if (target) {
        region_move_ms_ = now;
        if (set_rf_channel(target, false) != 0) {
            region_retry_ms_ = now_ms() + 10000;   // it searches all again by itself; another go later
            region_due = true;
            return;
        }
        search_all_channels();   // the ground follows the air unit again (the move pinned it)
        region_due = false;      // which noted a change: this is it
        cur = target;
    }
    if (!chan_hop.load()) {
        // AUTO: the air unit stays put. One we had hopping stops where it is.
        if (region_hop_told_) {
            printf("ar8030: AUTO: the air unit stops hopping, on %u kHz\n", cur);
            region_q_lists_.push_back(want);
            request_setting(CAM_WORK_LIST, (int)want.size());
            request_setting(CAM_HOP_OFF, (int)cur);
        }
        region_hop_told_ = false;
        region_hop_n = 0;
        return;
    }
    // AUTO HOP: the air unit hops by itself on the list, from where it is
    // (0x21 names that channel; kestrel-air does not move for it). Already
    // in this link's handshake: not again.
    const bool in_handshake = want == region_hs_list_;
    region_hs_list_.clear();
    if (in_handshake) return;
    std::string list;
    for (uint32_t f : want) list += " " + std::to_string(f / 1000);
    printf("ar8030: AUTO HOP in %s: the air unit hops by itself on %zu channels (%s ), from %u kHz\n",
           code, want.size(), list.c_str(), cur);
    region_q_lists_.push_back(want);
    request_setting(CAM_WORK_LIST, (int)want.size());
    request_setting(CAM_HOP_ON, (int)cur);
    region_hop_told_ = true;
    region_hop_n = (int)want.size();
}

// Experiment: `echo G > /tmp/bw.req` puts the video link on bandwidth gear G
// (bb_bandwidth_e, 0..5 = 1.25, 2.5, 5, 10, 20, 40 MHz; bb_config_*.json starts
// it, the "br" link, at 5 MHz = 2). The air unit is the AP and transmits the
// video, so it has to change first: sky cmd 0x24 with gear + 1 to kestrel-air
// (stock ignores it; its own handshake sends 0 at every link-up), then our
// receive side. The radio also widens the link by itself after link-up. No new picture 3 s later and both go back to the
// gear before - kestrel-air does that by itself when the link is gone, since we
// could not reach it then. Not saved.
void Ar8030Link::bw_poll() {
    static int cur = 2, prev = -1;
    static uint64_t last = 0, at = 0, frames_at = 0;
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    const uint64_t t = now_ms();
    if (!dev || t - last < 500) return;
    last = t;
    // The widest the link may go with a stock air radio (it drops and re-forms
    // the link at every step to or from 40 MHz) is our Open8030 radio's own
    // call: it keeps to 20 MHz until the air radio's frame stamps say Open8030.
    // Tests: /tmp/kestrel-regwrite holding "offset mask value" (hex) changes
    // those bits of one baseband register (0x0a110000 + offset) through
    // GET/SET 100, for A/B tests of radio settings. Logs before and after.
    if (FILE *f = fopen("/tmp/kestrel-regwrite", "r")) {
        unsigned off = 0, mask = 0, val = 0;
        const int n = fscanf(f, "%x %x %x", &off, &mask, &val);
        fclose(f);
        unlink("/tmp/kestrel-regwrite");
        if (n == 3 && off < 0x1000 && !(off & 3)) {
            uint8_t in[4] = {(uint8_t)off, (uint8_t)(off >> 8), 4, 0}, out[256] = {0};
            if (ar_ioctl(dev, 0x01000064, in, out) == 0) {
                uint32_t before = out[0] | out[1] << 8 | out[2] << 16 | (uint32_t)out[3] << 24;
                uint32_t after = (before & ~mask) | (val & mask);
                uint8_t w[260] = {(uint8_t)off, (uint8_t)(off >> 8), 4, 0,
                                  (uint8_t)after, (uint8_t)(after >> 8), (uint8_t)(after >> 16), (uint8_t)(after >> 24)};
                const int rc = ar_ioctl(dev, 0x02000064, w, NULL);
                printf("ar8030: /tmp/kestrel-regwrite 0x%03x: 0x%08x -> 0x%08x (rc %d)\n", off, before, after, rc);
            }
        }
    }
    // Tests: touching /tmp/kestrel-regdump writes the whole baseband register
    // block (0x0a110000..0x0a110fff, GET 100 in 256-byte reads) to
    // /tmp/kestrel-regs.txt, one "offset value" line per word, to diff radio
    // firmwares on a live link.
    if (access("/tmp/kestrel-regdump", F_OK) == 0) {
        unlink("/tmp/kestrel-regdump");
        if (FILE *o = fopen("/tmp/kestrel-regs.txt", "w")) {
            for (unsigned off = 0; off < 0x1000; off += 256) {
                uint8_t in[4] = {(uint8_t)off, (uint8_t)(off >> 8), 0, 1}, out[256] = {0};
                if (ar_ioctl(dev, 0x01000064, in, out) != 0) { fprintf(o, "0x%03x FAIL\n", off); continue; }
                for (unsigned w = 0; w < 256; w += 4)
                    fprintf(o, "0x%03x %08x\n", off + w,
                            out[w] | out[w + 1] << 8 | out[w + 2] << 16 | (uint32_t)out[w + 3] << 24);
            }
            fclose(o);
            printf("ar8030: /tmp/kestrel-regdump -> /tmp/kestrel-regs.txt\n");
        }
    }
    // Tests: /tmp/kestrel-chan holding a frequency in MHz moves the link there
    // exactly as OK on the channel page does; -1 hands it back to AUTO.
    if (FILE *f = fopen("/tmp/kestrel-chan", "r")) {
        int mhz = 0;
        if (fscanf(f, "%d", &mhz) != 1) mhz = 0;
        fclose(f);
        unlink("/tmp/kestrel-chan");
        if (mhz == -1 || (mhz >= 2000 && mhz <= 7000)) {
            printf("ar8030: /tmp/kestrel-chan: %d\n", mhz);
            request_rf(RF_CHAN, mhz < 0 ? -1 : mhz * 1000);
        }
    }
    auto set_ours = [&](int g) {
        bb_set_bandwidth_t p;
        memset(&p, 0, sizeof(p));
        p.slot = BB_SLOT_AP;
        p.dir = BB_DIR_RX;
        p.bandwidth = (uint8_t)g;
        return ar_ioctl(dev, BB_SET_BANDWIDTH, &p, NULL);
    };
    auto tell_air = [&](int g) {
        if (ctrl_sockfd < 0) return -1;
        // gear + 1: 0 is what stock's own handshake sends, and means "leave it".
        std::vector<uint8_t> f = sky_proto.build_u8(sky::CMD_SET_BB_BANDWIDTH, (uint8_t)(g + 1));
        return bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 200);
    };
    if (prev >= 0 && t - at >= 3000) {
        if (video_.frames_seen() == frames_at) {
            printf("ar8030: bandwidth gear %d: no picture in 3 s - back to gear %d\n", cur, prev);
            const int w = tell_air(prev);
            usleep(50000);
            printf("ar8030: bandwidth gear %d: air <- %d, ours -> %d\n", prev, w, set_ours(prev));
            cur = prev;
        } else {
            printf("ar8030: bandwidth gear %d kept: %llu pictures in 3 s\n", cur,
                   (unsigned long long)(video_.frames_seen() - frames_at));
        }
        prev = -1;
    }
    FILE *rq = fopen("/tmp/bw.req", "r");
    if (!rq) return;
    int g = -1;
    if (fscanf(rq, "%d", &g) != 1) g = -1;
    fclose(rq);
    unlink("/tmp/bw.req");
    if (g < 0 || g > 5 || prev >= 0 || g == cur) return;
    const int w = tell_air(g);
    usleep(50000);
    printf("ar8030: bandwidth gear %d -> %d: air <- %d, ours -> %d\n", cur, g, w, set_ours(g));
    prev = cur;
    cur = g;
    at = t;
    frames_at = video_.frames_seen();
}

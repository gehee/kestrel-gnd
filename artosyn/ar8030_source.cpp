#include "ar8030_source.hpp"
#include "bb_watchdog.hpp"
#include "ar8030_handshake.h"
#include "../settings.hpp"
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>

#include <set>
#include "../utils/scheduling_helper.hpp"

extern "C" {
#include "bb_client.h"

}

// artosyn/bb_sdk_shim.cpp - overrides the linked SDK's ioctl length table with
// the newer one extracted from stock ar_ldy_gnd.
extern "C" void bb_sdk_shim_report();

// Read chunk size. The baseband hands us H.265 in bursts; 256K keeps the
// syscall rate low without adding meaningful latency (a 1080p60 IDR at
// 40Mbit/s is ~80K, so a big frame still lands in one or two reads).
#define AR_READ_CHUNK (256 * 1024)

// Guard against a runaway accumulator if we never see a second start code
// (corrupt stream / wrong port). 4MB is far above any sane single NAL.
#define AR_MAX_ACCUM (4 * 1024 * 1024)

static inline uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static inline uint64_t now_ms() { return now_us() / 1000ULL; }

// ---- H.265 parameter-set sanity --------------------------------------------
//
// A two-byte header that looks right is not enough to trust a VPS/SPS/PPS: a
// 31-byte "SPS" with a valid header but garbage inside (it reads as SPS id 47,
// where 0-15 is the whole range) got cached next to the real one, went into
// every hvcC from then on, and the iPhone refused to decode the web stream at
// all. So the id is read and range-checked, and an SPS must also carry a
// chroma format and a picture size that exist.

namespace {
struct Rbsp {
    std::vector<uint8_t> b;
    size_t bit = 0;
    bool bad = false;
    Rbsp(const uint8_t* p, size_t n) {           // drop emulation-prevention bytes
        int zeros = 0;
        for (size_t i = 0; i < n; i++) {
            if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }
            b.push_back(p[i]);
            zeros = p[i] == 0 ? zeros + 1 : 0;
        }
    }
    uint32_t u(int n) {
        uint32_t v = 0;
        while (n--) {
            if (bit >= b.size() * 8) { bad = true; return 0; }
            v = (v << 1) | ((b[bit >> 3] >> (7 - (bit & 7))) & 1);
            bit++;
        }
        return v;
    }
    uint32_t ue() {
        int z = 0;
        while (!bad && u(1) == 0) if (++z > 31) { bad = true; return 0; }
        return z ? (1u << z) - 1 + u(z) : 0;
    }
};
}  // namespace

// The id of an H.265 VPS/SPS/PPS (payload after the 2-byte header), or -1 if
// the set cannot be real.
static int hevc_param_set_id(int type, const uint8_t* p, size_t n) {
    Rbsp r(p, n);
    if (type == 32) {                                   // VPS
        const uint32_t id = r.u(4);
        return r.bad ? -1 : (int)id;
    }
    if (type == 34) {                                   // PPS
        const uint32_t id = r.ue();
        return (r.bad || id > 63) ? -1 : (int)id;
    }
    if (type != 33) return -1;                          // SPS
    r.u(4);                                             // sps_video_parameter_set_id
    const uint32_t max_sub = r.u(3);                    // sps_max_sub_layers_minus1
    r.u(1);                                             // temporal_id_nesting
    if (max_sub > 6) return -1;
    // profile_tier_level(1, max_sub)
    r.u(88); r.u(8);                                    // general profile + level
    uint32_t prof[8] = {0}, lvl[8] = {0};
    for (uint32_t i = 0; i < max_sub; i++) { prof[i] = r.u(1); lvl[i] = r.u(1); }
    if (max_sub > 0) for (uint32_t i = max_sub; i < 8; i++) r.u(2);
    for (uint32_t i = 0; i < max_sub; i++) { if (prof[i]) r.u(88); if (lvl[i]) r.u(8); }
    const uint32_t id = r.ue();
    const uint32_t chroma = r.ue();
    if (chroma == 3) r.u(1);                            // separate_colour_plane_flag
    const uint32_t w = r.ue(), h = r.ue();
    if (r.bad || id > 15 || chroma > 3 || w < 16 || h < 16 || w > 8192 || h > 8192) return -1;
    return (int)id;
}

// Every bb_ioctl() in this SDK waits forever. The public entry point hard-codes
// timeout = -1 (ar8030.c:253), and -1 selects the untimed pthread_cond_wait in
// bs_send_usbpack_and_wait() rather than the timed one beside it. So a single
// lost reply from the daemon parks the calling thread for the life of the
// process - and this loop polls status several times a second, which is why
// the failure looks like "it connected and then stopped" rather than a failure
// to connect. Caught in the act as AR8030_RX sitting in futex_wait_queue_me
// with both sockets open and the log frozen.
//
// bb_ioctl_ex() is the same call with a caller-supplied deadline, exported by
// the same vendor library we already link, so this needs no patched SDK. A
// timeout comes back as a plain failure, which every caller here already
// handles - and a call that fails is recoverable where one that never returns
// is not.
static const int kIoctlTimeoutMs = 1000;
// Bring-up requests get longer than an ordinary ioctl: the daemon has real
// work to do behind BB_INIT_REQ, and a deadline that expires while it is
// simply busy would turn a slow start into a failed one.
static const int kBringupReqTimeoutMs = 3000;
// Consecutive failed bring-ups before we stop asking nicely and restart the
// daemon. Two is roughly 15s of trying, which is long enough to ride out a
// daemon that is merely slow to start and short enough that a stuck one is
// cleared before the pilot gives up on it.
static const int kFailsBeforeDaemonRestart = 2;

static int ar_ioctl(bb_dev_handle_t *dev, uint32_t request, const void *in, void *out) {
    int rc = bb_ioctl_ex(dev, request, in, out, kIoctlTimeoutMs);
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


Ar8030Source::Ar8030Source(const VideoCodec& codec_, std::shared_ptr<Vdec> vdec_,
                           std::shared_ptr<DVR> dvr_, std::shared_ptr<OSD> osd_,
                           volatile bool* stop_signal,
                           std::string host_, int host_port_,
                           int slot_, int video_port_)
    : codec(codec_), vdec(vdec_), dvr(dvr_), osd(osd_), should_stop(stop_signal),
      host(std::move(host_)), host_port(host_port_), slot(slot_), video_port(video_port_) {
    accum.reserve(AR_READ_CHUNK * 2);
}

Ar8030Source::~Ar8030Source() { disconnect_bb(); }


// Defaults taken from the stock boot log for this board (rf_board 16):
//   gui_fpv_bb_set_freq, freq=5740000 ; gui_fpv_bb_set_bandwidth, bandwidth=0
unsigned Ar8030Source::freq_khz = 5740000;
// -1 = leave the radio on whatever bandwidth it negotiates.
//
// Forcing this from the ground does NOT work and measurably hurts: the air unit
// is the AP and owns the channel, so a unilateral BB_SET_BANDWIDTH(20M) on the
// DEV side is accepted (returns 0) and then costs throughput - measured 3.06 ->
// 2.10 Mbps at the USB layer, MCS 10 -> 8.
int Ar8030Source::bandwidth = -1;
bool Ar8030Source::do_pair = false;
int Ar8030Source::ap_index = 0;
bool Ar8030Source::chan_auto = true;
int Ar8030Source::tx_power_dbm = 24;
int Ar8030Source::tx_power_mw  = 500;   // = 24 dBm, what stock boots with
bool Ar8030Source::tx_power_auto = false;
bool Ar8030Source::skip_handshake = false;
int Ar8030Source::msp_bb_port = 2;   // the FC stream rides the telemetry socket
int Ar8030Source::standby_mode = -1;
// Default OFF. Replaying stock's cmd138(4, 500) is accepted (rc=0) but on its
// own it COSTS throughput: measured 3.06 -> 2.09 Mbps at the USB layer, MCS 10
// -> 8, which is within noise of the BB_SET_BANDWIDTH(20M) result (2.10). Both
// widen the ground receiver while the air unit keeps transmitting narrow, so
// the extra noise bandwidth just lowers SNR. Enable only together with the
// air-side half below.
int Ar8030Source::air_bw        = -1;
int Ar8030Source::replay_stock_rf = 1;
int Ar8030Source::prj_rf_bw     = -1;
int Ar8030Source::prj_rf_pwr_mw = -1;
std::string Ar8030Source::dump_path;
int Ar8030Source::mode_w = 0;
int Ar8030Source::mode_h = 0;
int Ar8030Source::mode_fps = 0;
int Ar8030Source::mode_chn = 0;
std::atomic<int> Ar8030Source::pending_mode_index{-1};
bool Ar8030Source::probe_modes = false;
bool Ar8030Source::probe_rf = false;
std::atomic<bool> Ar8030Source::scan_active{false};
std::mutex Ar8030Source::pending_mtx;
std::vector<std::pair<int,int>> Ar8030Source::pending_settings;
std::vector<std::pair<int,int>> Ar8030Source::pending_rf;
std::atomic<int> Ar8030Source::bind_request{0};
std::atomic<int> Ar8030Source::bind_state{Ar8030Source::BIND_IDLE};
std::atomic<int> Ar8030Source::bind_secs_left{0};
std::atomic<unsigned> Ar8030Source::bind_peer{0};

void Ar8030Source::request_rf(int field, int value) {
    std::lock_guard<std::mutex> lk(pending_mtx);
    pending_rf.push_back({field, value});
}

void Ar8030Source::request_setting(int field, int value) {
    std::lock_guard<std::mutex> lk(pending_mtx);
    pending_settings.push_back({field, value});
}
bool Ar8030Source::decode_enabled = false;

// Parse the paired air-unit MACs out of /factory/user_cfg.json. The file is a
// flat object holding "bb_mac_addr_N": [ "0xE5", "0xDA", "0x00", "0x08" ], the
// same values the stock firmware logged as candi_mac[N].
static int ar_load_candidate_macs(bb_mac_t *out, int max) {
    FILE *f = fopen("/factory/user_cfg.json", "r");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    int count = 0;
    char *p = buf;
    while (count < max && (p = strstr(p, "bb_mac_addr_")) != NULL) {
        char *lb = strchr(p, '[');
        if (!lb) break;
        char *rb = strchr(lb, ']');
        if (!rb) break;
        *rb = 0;
        unsigned vals[BB_MAC_LEN];
        int got = 0;
        char *q = lb + 1;
        while (got < BB_MAC_LEN) {
            char *hx = strstr(q, "0x");
            if (!hx) break;
            vals[got++] = (unsigned)strtoul(hx, &q, 16);
        }
        *rb = ']';
        if (got == BB_MAC_LEN) {
            for (int i = 0; i < BB_MAC_LEN; i++)
                out[count].addr[i] = (uint8_t)vals[i];
            count++;
        }
        p = rb;
    }
    return count;
}


// Events stock subscribes to before touching a socket (capture: class 03 01,
// args 01, 00, 02, 08, 0C). 0=LINK_STATE 1=MCS_CHANGE 2=CHAN_CHANGE
// 8=PRJ_DISPATCH2 (custom dispatch over USB RPC) 12=MAX.
static void ar_event_cb(void *arg, void *user) {
    (void)arg;
    (void)user;   // we only need the subscription to exist, not to act on it
}

void Ar8030Source::subscribe_events() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    static const int evts[] = { 1, 0, 2, 8, 12 };   // stock's order
    for (size_t i = 0; i < sizeof(evts) / sizeof(evts[0]); i++) {
        bb_set_event_callback_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = (bb_event_e)evts[i];
        ev.callback = ar_event_cb;
        ev.user = NULL;
        printf("ar8030: subscribe event %d -> %d\n", evts[i],
               ar_ioctl(dev, BB_SET_EVENT_SUBSCRIBE, &ev, NULL));
    }
}

void Ar8030Source::configure_link() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;

    bb_mac_t macs[BB_CONFIG_MAX_SLOT_CANDIDATE];
    int n = ar_load_candidate_macs(macs, BB_CONFIG_MAX_SLOT_CANDIDATE);
    if (n > 0) {
        for (int i = 0; i < n; i++)
            printf("ar8030: candidate[%d] = %02X:%02X:%02X:%02X\n", i,
                   macs[i].addr[0], macs[i].addr[1], macs[i].addr[2], macs[i].addr[3]);
        bb_set_candidate_t cand;
        memset(&cand, 0, sizeof(cand));
        cand.slot = BB_SLOT_0;
        cand.mac_num = (uint8_t)n;
        memcpy(cand.mac_tab, macs, sizeof(bb_mac_t) * n);
        printf("ar8030: BB_SET_CANDIDATES -> %d\n", ar_ioctl(dev, BB_SET_CANDIDATES, &cand, NULL));
    } else {
        // Empty on a goggle never paired under stock (fpvOS merges stock's
        // list off the NAND at boot) and never bound here: not a fault, just
        // nothing to link with yet.
        printf("ar8030: no paired air units in /factory/user_cfg.json - bind one from the menu\n");
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
            int idx = (ap_index >= 0 && ap_index < n) ? ap_index : 0;
            set_sky_key_from_mac(macs[idx].addr);   // per-VTX settings namespace
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
    rf_caps |= RF_CAP_BW;

    // MCS mode and LNA are set to the values we want anyway (auto/auto), so
    // this doubles as a harmless capability check.
    bb_set_mcs_mode_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.auto_mode = 1;
    int rc_mcs = ar_ioctl(dev, BB_SET_MCS_MODE, &mm, NULL);
    printf("ar8030: BB_SET_MCS_MODE(auto=1) -> %d\n", rc_mcs);
    if (rc_mcs == 0) rf_caps |= RF_CAP_MCS;

    bb_set_lna_mode_t ln;
    memset(&ln, 0, sizeof(ln));
    ln.mode = -1;                       // -1 = automatic gain
    int rc_lna = ar_ioctl(dev, BB_SET_LNA_MODE, &ln, NULL);
    printf("ar8030: BB_SET_LNA_MODE(auto) -> %d\n", rc_lna);
    if (rc_lna == 0) rf_caps |= RF_CAP_LNA;

    // Channel adaptation: with auto_mode=1 the baseband scans/hops to find its
    // AP, which is what we want when we do not know the air unit's channel.
    // Only in manual mode does an explicit BB_SET_FREQ mean anything.
    bb_set_chan_mode_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.auto_mode = chan_auto ? 1 : 0;
    int rc_cm = ar_ioctl(dev, BB_SET_CHAN_MODE, &cm, NULL);
    printf("ar8030: BB_SET_CHAN_MODE(auto=%d) -> %d\n", cm.auto_mode, rc_cm);
    if (rc_cm == 0) rf_caps |= RF_CAP_CHAN;

    enable_ranging();

    printf("ar8030: RF capabilities: power=%d mcs=%d lna=%d bandwidth=%d chan=%d\n",
           !!(rf_caps & RF_CAP_POWER), !!(rf_caps & RF_CAP_MCS),
           !!(rf_caps & RF_CAP_LNA),   !!(rf_caps & RF_CAP_BW),
           !!(rf_caps & RF_CAP_CHAN));
    if (osd) osd->set_rf_caps(rf_caps);

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


// ---- binding ---------------------------------------------------------------
//
// Ask the radio to listen for an air unit announcing itself. bind_begin()
// opens the window, bind_step() polls it, bind_finish() closes it and - on
// success - writes the peer into /factory/user_cfg.json so the pairing is
// still there after a reboot.

bool Ar8030Source::bind_begin() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) {
        printf("ar8030: bind requested but the baseband is not open yet\n");
        return false;
    }
    // Stock reads the role first and uses it to decide what to write once a
    // peer is found: a DEV takes BB_SET_AP_MAC, an AP takes BB_SET_CANDIDATES.
    // Read it here rather than at the end, so a status failure aborts before
    // the radio is put into pairing mode.
    {
        bb_get_status_in_t sin;
        memset(&sin, 0, sizeof(sin));
        sin.user_bmp = 0;                       // stock passes 0 here
        bb_get_status_out_t sout;
        memset(&sout, 0, sizeof(sout));
        if (ar_ioctl(dev, BB_GET_STATUS, &sin, &sout) != 0) {
            printf("ar8030: bind aborted - BB_GET_STATUS failed\n");
            return false;
        }
        bind_role_ = sout.role;
        printf("ar8030: prepare pair start slot_bmp=0x%02x role=%s\n",
               1 << BB_SLOT_0, bind_role_ == BB_ROLE_AP ? "AP" : "DEV");
    }

    // Do NOT clear the candidate list or the pinned AP here.
    //
    // An earlier version did, on the theory that our short 22-byte
    // BB_SET_CANDIDATES struct had left garbage in the chip. That reasoning
    // was sound but the cure was worse: a bare-RPC pair window that skips
    // both clears pairs within seconds, every time, while kestrel with the
    // clears in place never caught anything across five attempts on real
    // hardware. Stock does not clear either. The likeliest reading is that
    // mac_num = 0 means "accept nobody" on this firmware rather than "accept
    // anybody", which turns the pair window into a window onto nothing.
    //
    // Set KESTREL_BIND_CLEAR=1 to put the clears back for comparison.
    const char *doclear = getenv("KESTREL_BIND_CLEAR");
    if (doclear && doclear[0] == '1') {
        bb_conf_candidates_t none;
        memset(&none, 0, sizeof(none));
        none.slot    = BB_SLOT_0;
        none.mac_num = 0;
        printf("ar8030: clear candidates for bind -> %d\n",
               ar_ioctl(dev, BB_SET_CANDIDATES, &none, NULL));
        bb_set_ap_mac_t clear;
        memset(&clear, 0, sizeof(clear));
        printf("ar8030: clear AP MAC for bind -> %d\n",
               ar_ioctl(dev, BB_SET_AP_MAC, &clear, NULL));
    }

    memset(bind_seen_, 0, sizeof(bind_seen_));
    memset(bind_seen_n_, 0, sizeof(bind_seen_n_));
    {
        const uint64_t t = now_ms();
        bind_had_data_ = (last_ctrl_ms && t - last_ctrl_ms < 3000) ||
                         (last_data_ms && t - last_data_ms < 3000);
        if (bind_had_data_)
            printf("ar8030: an air unit is already talking - binding anyway "
                   "(a second unit); traffic will not stop this window\n");
    }
    // Start the idle-read counter fresh. An unbound goggle has usually been
    // sitting at "no video" for a long time already, so without this the
    // teardown timer could be one read away from firing when the window ends.
    read_timeouts = 0;

    bb_set_pair_mode_t pm;
    memset(&pm, 0, sizeof(pm));
    pm.start = 1;
    pm.slot_bmp = (1 << BB_SLOT_0);
    int r = ar_ioctl(dev, BB_SET_PAIR_MODE, &pm, NULL);
    printf("ar8030: BB_SET_PAIR_MODE(start) -> %d\n", r);
    if (r != 0) return false;

    printf("ar8030: BINDING - hold the air unit's bind button until its LED turns\n"
           "        RED, which is its pairing mode (%ds to do it)\n",
           kBindWindowS);
    bind_started_ms_   = now_ms();
    bind_next_poll_ms_ = bind_started_ms_ + 500;
    bind_done_ms_      = 0;
    bind_secs_left.store(kBindWindowS);
    bind_state.store(BIND_RUNNING);
    if (osd) osd->signal_render(prof::kWakeLink);
    return true;
}

void Ar8030Source::bind_finish(bool ok, const uint8_t peer[4], bool cancelled) {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (dev) {
        bb_set_pair_mode_t pm;
        memset(&pm, 0, sizeof(pm));
        pm.start = 0;
        pm.slot_bmp = (1 << BB_SLOT_0);
        printf("ar8030: BB_SET_PAIR_MODE(stop) -> %d\n",
               ar_ioctl(dev, BB_SET_PAIR_MODE, &pm, NULL));
    }
    bind_done_ms_ = now_ms();
    bind_secs_left.store(0);

    if (cancelled) {
        // The user's own doing, not an outcome: nothing to report and nothing
        // to hold on screen. Straight back to idle, window closed.
        bind_peer.store(0);
        bind_state.store(BIND_IDLE);
        bind_done_ms_ = 0;
        if (osd) osd->signal_render(prof::kWakeLink);
        report_link_status();
        return;
    }

    if (ok && peer) {
        bind_peer.store(((unsigned)peer[0] << 24) | ((unsigned)peer[1] << 16) |
                        ((unsigned)peer[2] << 8)  |  (unsigned)peer[3]);
        bind_state.store(BIND_OK);
        if (osd) osd->signal_render(prof::kWakeLink);

        // Program the peer the way stock does, which depends on which side we
        // are. A DEV is told the one AP to associate with; an AP is given a
        // candidate list of DEVs allowed to join it. The goggle is always the
        // DEV, but the branch costs nothing and documents the asymmetry.
        if (dev) {
            if (bind_role_ == BB_ROLE_AP) {
                bb_conf_candidates_t cand;
                memset(&cand, 0, sizeof(cand));
                cand.slot    = BB_SLOT_0;
                cand.mac_num = 1;
                memcpy(cand.mac_tab[0].addr, peer, BB_MAC_LEN);
                printf("ar8030: BB_SET_CANDIDATES(peer) -> %d\n",
                       ar_ioctl(dev, BB_SET_CANDIDATES, &cand, NULL));
            } else {
                bb_set_ap_mac_t ap;
                memset(&ap, 0, sizeof(ap));
                memcpy(ap.mac.addr, peer, BB_MAC_LEN);
                printf("ar8030: BB_SET_AP_MAC(%02X:%02X:%02X:%02X) -> %d\n",
                       peer[0], peer[1], peer[2], peer[3],
                       ar_ioctl(dev, BB_SET_AP_MAC, &ap, NULL));
            }
        }

        if (save_paired_mac(peer))
            printf("ar8030: bound peer saved to /factory/user_cfg.json\n");
        else
            printf("ar8030: WARNING could not save the bind - it will be lost on reboot\n");
    } else {
        bind_peer.store(0);
        bind_state.store(BIND_FAILED);
        if (osd) osd->signal_render(prof::kWakeLink);
        printf("ar8030: binding timed out after %ds - no air unit answered.\n"
               "        The air unit only announces itself in ITS pairing mode:\n"
               "        hold its bind button until the LED turns RED, then retry.\n",
               kBindWindowS);
    }
    report_link_status();
}

void Ar8030Source::bind_step() {
    const uint64_t now = now_ms();
    int st = bind_state.load();

    // Let a finished result sit on screen a few seconds, then clear it.
    if ((st == BIND_OK || st == BIND_FAILED || st == BIND_LINKED) && bind_done_ms_ &&
        now - bind_done_ms_ > (uint64_t)kBindResultHoldMs) {
        bind_state.store(BIND_IDLE);
        bind_done_ms_ = 0;
        st = BIND_IDLE;
        if (osd) osd->signal_render(prof::kWakeLink);
    }

    // A request parked by the menu or the front-panel button.
    if (bind_request.exchange(0) == 1) {
        if (st == BIND_RUNNING) {
            // Pressing again while a window is open cancels it. With a
            // two-minute window there has to be a way out other than waiting
            // it out or pulling the power.
            printf("ar8030: bind cancelled by a second press\n");
            bind_finish(false, NULL, true);
            return;
        }
        if (!bind_begin()) {
            bind_state.store(BIND_FAILED);
            bind_done_ms_ = now;
            if (osd) osd->signal_render(prof::kWakeLink);
        }
        return;
    }

    if (bind_state.load() != BIND_RUNNING) return;

    // NOT "close the window when data arrives". That was tried and it cannot
    // work: BB_SET_PAIR_MODE suspends normal association at the radio, so a
    // known air unit does not link and no data flows for as long as the
    // window is open. Confirmed on hardware - the link only comes up once
    // binding is cancelled. Any rule keyed to traffic is dead code here.
    //
    // What the window costs is therefore a paused link, which is why the
    // caption says so and why a second press cancels.

    int elapsed = (int)((now - bind_started_ms_) / 1000);
    int left = kBindWindowS - elapsed;
    if (left < 0) left = 0;
    // Repaint when the displayed second changes. The OSD renders on frame
    // arrival, and during a bind there is by definition no video yet, so
    // without this the countdown would freeze at 30 on screen.
    if (left != bind_secs_left.exchange(left) && osd) osd->signal_render(prof::kWakeLink);

    // Stock polls every 20ms. We poll as often as the RX loop comes round,
    // capped at that rate; the stability rule below is what matters, not the
    // exact cadence.
    if (now < bind_next_poll_ms_) return;
    bind_next_poll_ms_ = now + 20;

    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (dev) {
        // in = NULL: the daemon's length table gives BB_GET_PAIR_RESULT an
        // input length of 0, and stock passes a null input pointer. out is
        // 164 bytes; the struct is now exactly that (see bb_client.h), but
        // the oversized buffer stays as belt and braces against a daemon that
        // writes more than its table claims.
        alignas(8) uint8_t pr_buf[256];
        memset(pr_buf, 0, sizeof(pr_buf));
        bb_get_pair_out_t& pr = *reinterpret_cast<bb_get_pair_out_t*>(pr_buf);
        if (ar_ioctl(dev, BB_GET_PAIR_RESULT, NULL, pr_buf) == 0 && pr.slot_bmp) {
            // Stock does not accept the first MAC it sees. It requires the
            // same address to come back unchanged 11 polls running, and
            // restarts the count whenever it changes. Early in a window the
            // radio reports addresses that then vanish, so taking the first
            // one binds to noise.
            for (int i = 0; i < BB_SLOT_MAX; i++) {
                if (!(pr.slot_bmp & (1 << i))) continue;
                if (memcmp(bind_seen_[i], pr.peer_mac[i].addr, BB_MAC_LEN) != 0) {
                    memcpy(bind_seen_[i], pr.peer_mac[i].addr, BB_MAC_LEN);
                    bind_seen_n_[i] = 1;
                    // Log the first sighting of any address. When a bind
                    // fails this separates "the radio heard nothing" from
                    // "the radio heard something and we rejected it", which
                    // are entirely different bugs.
                    printf("ar8030: bind sees slot%d %02X:%02X:%02X:%02X\n", i,
                           bind_seen_[i][0], bind_seen_[i][1],
                           bind_seen_[i][2], bind_seen_[i][3]);
                } else if (bind_seen_n_[i] < 1000) {
                    bind_seen_n_[i]++;
                }
            }
            const int slot = BB_SLOT_0;
            if (bind_seen_n_[slot] > kBindStableReads) {
                printf("ar8030: BOUND! slot_bmp=0x%02x peer=%02X:%02X:%02X:%02X"
                       " (stable over %d reads)\n",
                       pr.slot_bmp, bind_seen_[slot][0], bind_seen_[slot][1],
                       bind_seen_[slot][2], bind_seen_[slot][3],
                       bind_seen_n_[slot]);
                bind_finish(true, bind_seen_[slot]);
                return;
            }
        }
    }
    if (left <= 0) bind_finish(false, NULL);
}

// Write the bound air unit into /factory/user_cfg.json, the way stock does.
//
// Stock's save_ap_candidate() keeps a 100-entry RING, not a single peer:
// "bb_mac_addr_0".."bb_mac_addr_99", each a JSON array of four "0xNN" strings,
// with "save_candidate_position" holding the next slot to write and advancing
// (pos + 1) % 100. A MAC already present anywhere in the ring is left alone
// rather than rewritten - that is why re-binding the same air unit does not
// churn the file. Matching this matters because the whole ring is what gets
// pushed back to the radio as the candidate list.
//
// There is no JSON library here, so the file is parsed with string scans and
// rewritten whole. Unknown keys are not preserved; the stock file has only
// these three, and inventing others would be worse than dropping them.
bool Ar8030Source::save_paired_mac(const uint8_t mac[4]) {
    static const char *kPath = "/factory/user_cfg.json";
    static const char *kTmp  = "/factory/.user_cfg.json.new";
    static const int   kRing = 100;

    int     pwr_cal = 0, save_pos = 0;
    uint8_t ring[kRing][BB_MAC_LEN];
    bool    have[kRing];
    memset(ring, 0, sizeof(ring));
    memset(have, 0, sizeof(have));

    if (FILE *f = fopen(kPath, "r")) {
        std::vector<char> buf(64 * 1024);
        size_t n = fread(buf.data(), 1, buf.size() - 1, f);
        buf[n] = 0;
        fclose(f);
        const char *b = buf.data();
        if (const char *p = strstr(b, "\"gnd_power_cal_dbm\""))
            if (const char *c = strchr(p, ':')) pwr_cal = atoi(c + 1);
        if (const char *p = strstr(b, "\"save_candidate_position\""))
            if (const char *c = strchr(p, ':')) save_pos = atoi(c + 1);
        for (int i = 0; i < kRing; i++) {
            char key[32];
            snprintf(key, sizeof(key), "\"bb_mac_addr_%d\"", i);
            const char *p = strstr(b, key);
            if (!p) continue;
            const char *q = strchr(p, '[');
            if (!q) continue;
            // Stop at the closing bracket so a short array cannot run on into
            // the next entry and silently borrow its bytes.
            const char *endq = strchr(q, ']');
            int got = 0;
            while (q && got < BB_MAC_LEN) {
                const char *hx = strstr(q, "0x");
                if (!hx || (endq && hx > endq)) break;
                char *end = NULL;
                ring[i][got++] = (uint8_t)strtoul(hx, &end, 16);
                q = end;
            }
            have[i] = (got == BB_MAC_LEN);
        }
    }

    // Already known? Stock says "mac same, not need save." and leaves the file
    // untouched, so re-binding the same air unit is a no-op on disk.
    for (int i = 0; i < kRing; i++)
        if (have[i] && memcmp(ring[i], mac, BB_MAC_LEN) == 0) {
            printf("ar8030: %02X:%02X:%02X:%02X already saved (slot %d)\n",
                   mac[0], mac[1], mac[2], mac[3], i);
            return true;
        }

    if (save_pos < 0 || save_pos >= kRing) save_pos = 0;
    memcpy(ring[save_pos], mac, BB_MAC_LEN);
    have[save_pos] = true;
    int next_pos = (save_pos + 1) % kRing;

    mkdir("/factory", 0755);          // harmless if it is already there
    FILE *f = fopen(kTmp, "w");
    if (!f) {
        printf("ar8030: cannot write %s: %s\n", kTmp, strerror(errno));
        return false;
    }
    fprintf(f, "{\n    \"gnd_power_cal_dbm\": %d,\n"
               "    \"save_candidate_position\": %d", pwr_cal, next_pos);
    for (int i = 0; i < kRing; i++) {
        if (!have[i]) continue;
        fprintf(f, ",\n    \"bb_mac_addr_%d\": [\n"
                   "        \"0x%02X\",\n        \"0x%02X\",\n"
                   "        \"0x%02X\",\n        \"0x%02X\"\n    ]",
                i, ring[i][0], ring[i][1], ring[i][2], ring[i][3]);
    }
    fprintf(f, "\n}\n");
    // The identity file is the one thing that must never end up truncated, so
    // it is written beside the original and moved over it only once complete.
    bool ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    fclose(f);
    if (!ok || rename(kTmp, kPath) != 0) {
        printf("ar8030: cannot replace %s: %s\n", kPath, strerror(errno));
        unlink(kTmp);
        return false;
    }
    chmod(kPath, 0600);               // stock ships it owner-only
    sync();                           // stock sync()s after saving; so do we
    printf("ar8030: saved %02X:%02X:%02X:%02X at ring slot %d (next %d)\n",
           mac[0], mac[1], mac[2], mac[3], save_pos, next_pos);
    return true;
}



// ---- per-air-unit camera settings -----------------------------------------
//
// Keyed by the AP MAC we associate with, so a second paired VTX keeps its own
// settings. The MAC is known from /factory/user_cfg.json before the link is up,
// which is what we need: the SET_CONFIG frames go out during the handshake.
void Ar8030Source::set_sky_key_from_mac(const uint8_t mac[4]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "sky_%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
    sky_cfg_key = buf;
}

void Ar8030Source::load_sky_config() {
    // Seed from the stock capture so every field we do not persist keeps the
    // value stock boots with, then let stored settings override.
    const uint8_t *f = ar_hs_5;
    unsigned lf = f[4] | (f[5] << 8);
    sky::read_config_payload(f + 7, (lf >> 4) - 1, &sky_cfg);
    if (sky_cfg_key.empty()) return;

    Settings &st = Settings::getInstance();
    const std::string k = sky_cfg_key + "_";
    sky_cfg.ch0_w       = (uint16_t)st.getInt(k + "ch0_w",  sky_cfg.ch0_w);
    sky_cfg.ch0_h       = (uint16_t)st.getInt(k + "ch0_h",  sky_cfg.ch0_h);
    sky_cfg.ch0_fps     = (uint8_t) st.getInt(k + "ch0_fps", sky_cfg.ch0_fps);
    sky_cfg.ev_x10      = (int8_t)  st.getInt(k + "ev",      sky_cfg.ev_x10);
    sky_cfg.scenes      = (uint8_t) st.getInt(k + "scene",   sky_cfg.scenes);
    sky_cfg.saturation  = (uint8_t) st.getInt(k + "sat",     sky_cfg.saturation);
    sky_cfg.sharpness   = (uint8_t) st.getInt(k + "sharp",   sky_cfg.sharpness);
    sky_cfg.contrast    = (uint8_t) st.getInt(k + "contrast", sky_cfg.contrast);
    sky_cfg.awb_cct     = (uint16_t)st.getInt(k + "awb",     sky_cfg.awb_cct);
    sky_cfg.anti_flicker= (uint8_t) st.getInt(k + "flicker", sky_cfg.anti_flicker);
    sky_cfg.angle       = (uint8_t) st.getInt(k + "flip",    sky_cfg.angle);
    sky_cfg.focus_en    = (uint8_t) st.getInt(k + "focus",   sky_cfg.focus_en);
    printf("ar8030: [%s] restored camera cfg: ch0=%ux%u@%u ev=%d sat=%u sharp=%u "
           "contrast=%u scene=%u awb=%u flicker=%u angle=%u focus=%u\n",
           sky_cfg_key.c_str(), sky_cfg.ch0_w, sky_cfg.ch0_h, sky_cfg.ch0_fps,
           sky_cfg.ev_x10, sky_cfg.saturation, sky_cfg.sharpness, sky_cfg.contrast,
           sky_cfg.scenes, sky_cfg.awb_cct, sky_cfg.anti_flicker, sky_cfg.angle,
           sky_cfg.focus_en);

    if (osd) {
        osd->set_camera_config(sky_cfg.ev_x10, sky_cfg.saturation, sky_cfg.contrast,
                               sky_cfg.sharpness, sky_cfg.scenes,
                               sky_cfg.awb_cct, sky_cfg.angle,
                               sky_cfg.dnr_3d, sky_cfg.focus_en);
    }

    // Point the menu row at the mode we are actually about to push. main()
    // publishes the list before the link exists, so without this the row shows
    // the built-in default no matter what the air unit is running.
    if (osd) {
        for (int i = 0; i < sky::kFpvModeCount; i++) {
            if (sky::kFpvModes[i].w == sky_cfg.ch0_w &&
                sky::kFpvModes[i].h == sky_cfg.ch0_h &&
                sky::kFpvModes[i].fps == sky_cfg.ch0_fps) {
                osd->set_video_mode_current(i);
                break;
            }
        }
    }
}

void Ar8030Source::save_sky_config() {
    if (sky_cfg_key.empty()) return;
    Settings &st = Settings::getInstance();
    const std::string k = sky_cfg_key + "_";
    st.set(k + "ch0_w",   (int)sky_cfg.ch0_w);
    st.set(k + "ch0_h",   (int)sky_cfg.ch0_h);
    st.set(k + "ch0_fps", (int)sky_cfg.ch0_fps);
    st.set(k + "ev",      (int)sky_cfg.ev_x10);
    st.set(k + "scene",   (int)sky_cfg.scenes);
    st.set(k + "sat",     (int)sky_cfg.saturation);
    st.set(k + "sharp",   (int)sky_cfg.sharpness);
    st.set(k + "contrast",(int)sky_cfg.contrast);
    st.set(k + "awb",     (int)sky_cfg.awb_cct);
    st.set(k + "flicker", (int)sky_cfg.anti_flicker);
    st.set(k + "focus",   (int)sky_cfg.focus_en);
}

// Rebuild one captured SET_CONFIG frame around our live settings. The stock
// body is the template, so the bytes we have not identified keep stock values;
// only the known fields and the timestamp change.
// Note for anyone re-sending a config frame outside the handshake: give it a
// fresh sequence number. Replaying the captured seq makes the air treat the
// frame as a duplicate and drop it silently - it is not acked and has no
// effect. Proved while chasing 3D DNR (references/sky-commands.md).
std::vector<uint8_t> Ar8030Source::build_config_frame(const uint8_t *tmpl, size_t len) {
    unsigned lf = tmpl[4] | (tmpl[5] << 8);
    size_t bodylen = (lf >> 4) - 1;
    std::vector<uint8_t> body(tmpl + 7, tmpl + 7 + bodylen);
    sky::patch_config_payload(body.data(), body.size(), sky_cfg);
    sky_proto.set_seq(tmpl[3]);                       // replaying stock's handshake
    return sky_proto.build(tmpl[6], body.data(), body.size());
}

// The air unit stays silent until the ground unit sends these frames on the
// port-2 (telemetry) socket - captured from a clean boot of the stock app.
// Opening the video socket alone yields at most a short burst of whatever was
// already buffered.
void Ar8030Source::send_air_handshake() {
    // The acquiring screen distinguishes "link up, nothing asked for yet"
    // from "asked, nothing came back", which is the difference between
    // waiting and being ignored.
    if (osd) osd->notify_handshake_sent();
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

    load_sky_config();
    for (size_t i = 0; i < ar_handshake_n; i++) {
        const uint8_t *data = ar_handshake[i].data;
        size_t         len  = ar_handshake[i].len;
        // The two SET_CONFIG frames carry the camera settings the VTX will run
        // with, so build them from our stored config instead of replaying the
        // capture - otherwise a menu change is undone at the next handshake.
        std::vector<uint8_t> rebuilt;
        if (data[6] == sky::CMD_SET_CONFIG) {
            rebuilt = build_config_frame(data, len);
            data = rebuilt.data(); len = rebuilt.size();
        }
        int w = bb_socket_write(ctrl_sockfd, data, (uint32_t)len, 500);
        printf("ar8030: handshake[%zu] seq=0x%02X len=%zu -> %d%s\n",
               i, data[3], len, w,
               rebuilt.empty() ? "" : "   (video config, from stored settings)");
        usleep(250000);  // spread the PA burst: back-to-back TX appears to brown the board out
        if (*should_stop) { printf("ar8030: handshake aborted (stopping)\n"); return; }
    }

    // The replayed frames end at seq 0x09; anything we synthesise from here on
    // has to carry on from there or the air unit drops it as out of order.
    sky_proto.set_seq(ar_handshake[ar_handshake_n - 1].data[3] + 1);
    if (probe_rf) probe_rf_ioctls();
    if (probe_modes) probe_video_modes();
    apply_video_mode();
}

// Read frames off the port-2 socket until one acknowledges want_cmd. The air
// unit interleaves unsolicited telemetry here, so we skip anything else.
bool Ar8030Source::read_sky_ack(uint8_t want_cmd, int timeout_ms) {
    if (ctrl_sockfd < 0) return false;
    uint8_t buf[512];
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (*should_stop) return false;
        int n = bb_socket_read(ctrl_sockfd, buf, sizeof(buf), 50);
        if (n <= 0) continue;   // -1 is a plain timeout, not an error
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

// The air unit pushes telemetry onto the port-2 socket continuously. Nothing
// consumed it after the startup handshake, so the SDK's per-socket RX ring
// (SOCK_LEN_APP_TO_DAEMON, 256 KB) filled up and every later packet was dropped
// by so_rpc_cb() with "recv data leak!! get = N , push= 0". Drain it every pass.
//
// NB: bb_socket_read() blocks forever on timeout <= 0 (it falls through to a
// bare pthread_cond_wait), so the poll below must pass a positive timeout.
void Ar8030Source::drain_control_socket() {
    if (ctrl_sockfd < 0) return;
    uint8_t buf[1024];
    for (int i = 0; i < 64; i++) {          // bounded: never starve the video read
        int n = bb_socket_read(ctrl_sockfd, buf, sizeof(buf), 1);
        if (n <= 0) break;                  // -1 here is just "ring empty"
        ctrl_bytes += (unsigned long long)n;
        last_ctrl_ms = now_ms();            // the air unit is talking to us
        scan_air_status(buf, n);

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

        // --debug-msp-bb-port 2 means "the FC stream is inside the telemetry we
        // already drain" - tee it rather than opening a second socket on a
        // port this client already owns.
        if (msp_bb_port == 2 && osd) {
            msp_bytes += (unsigned long long)n;
            osd->update_msp_data(buf, (size_t)n);
        }
    }
}

void Ar8030Source::scan_air_status(const uint8_t *buf, int n) {
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
            if (osd) osd->set_vtx_temp((float)vtx_cpu);

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
            if (osd) osd->set_vtx_low_power(low_power);

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
                    last_air_w = air.ch0_w; last_air_h = air.ch0_h;
                    last_air_fps = air.ch0_fps;
                    last_air_dnr3d = air.dnr_3d;
                    last_air_focus = air.focus_en;
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

        int sb = sky::Proto::find_tlv(pl, pl_len, sky::Proto::TLV_STANDBY_MODE);
        if (sb >= 0) {
            bool now = (sb != 0);
            if (!air_status_seen || now != air_standby) {
                printf("ar8030: air unit standby/low-power = %s\n", now ? "ON" : "OFF");
                air_standby = now;
                air_status_seen = true;
                if (osd) osd->set_air_standby(now);
            }
        }

        i += len - 1;
    }
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

void Ar8030Source::replay_stock_rf_setup() {
    if (!replay_stock_rf) { printf("ar8030: stock RF setup replay disabled\n"); return; }
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;

    int ok = 0, fail = 0;
    for (size_t i = 0; i < sizeof(kStockRfSetup) / sizeof(kStockRfSetup[0]); i++) {
        const StockSet &s = kStockRfSetup[i];
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, s.data, (size_t)s.len);
        int req = (2 << 24) | s.order;          // BB_REQUEST(BB_REQ_SET, order)
        int rc = ar_ioctl(dev, req, buf, NULL);
        if (rc == 0) ok++; else fail++;
        printf("ar8030: stock SET %-3d ->%3d  [", s.order, rc);
        for (int j = 0; j < s.len; j++) printf(" %02X", s.data[j]);
        printf(" ]\n");
    }
    printf("ar8030: stock RF setup replay: %d accepted, %d refused\n", ok, fail);
}

void Ar8030Source::send_prj_rf_config() {
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
// What to tell the air unit. Stock's "+1 means auto" encoding really is what
// goes on the wire: a bbproxy capture of stock caught cmd 0x22 carrying 101 for
// its "100mW Auto" menu entry. We used to strip the +1, which asked the air for
// a fixed level and silently disabled its adaptation. See
// references/sky-commands.md.
static int sky_pwr_mw(const ar_pwr_level &lv) {
    return lv.mw;                 // kArPwrLevels already stores AUTO as N+1
}

void Ar8030Source::apply_tx_power(int mw) {
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
    if (rc_pwr == 0) rf_caps |= RF_CAP_POWER;

    // Mirror it to the air unit. Guarded because configure_link() calls this
    // before the control socket exists; the post-handshake call covers startup.
    if (ctrl_sockfd >= 0) request_setting(CAM_PWR, sky_pwr_mw(lv));
}

void Ar8030Source::apply_bandwidth() {
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

void Ar8030Source::drain_msp_socket() {
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
        int n = bb_socket_read(msp_sockfd, buf, sizeof(buf), 1);
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
        if (osd) osd->update_msp_data(buf, (size_t)n);
    }
}

void Ar8030Source::drain_video_socket() {
    if (sockfd < 0) return;
    std::vector<uint8_t> tmp(16384);
    for (int i = 0; i < 64; i++) {
        int n = bb_socket_read(sockfd, tmp.data(), (uint32_t)tmp.size(), 1);
        if (n <= 0) break;
    }
}

// Wait for an ack of want_cmd, draining video meanwhile.
// Returns the ack status (0 = accepted, >0 = refused), or -1 on timeout.
int Ar8030Source::wait_sky_status(uint8_t want_cmd, int timeout_ms) {
    uint8_t buf[512];
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (*should_stop) return -1;
        drain_video_socket();
        int n = bb_socket_read(ctrl_sockfd, buf, sizeof(buf), 20);
        if (n <= 0) continue;
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
void Ar8030Source::apply_pending_settings() {
    std::vector<std::pair<int,int>> todo;
    {
        std::lock_guard<std::mutex> lk(pending_mtx);
        if (pending_settings.empty()) return;
        todo.swap(pending_settings);
    }
    if (ctrl_sockfd < 0) return;
    for (size_t i = 0; i < todo.size(); i++) {
        uint8_t cmd; const char* name;
        switch (todo[i].first) {
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
            case CAM_PWR:       cmd = sky::CMD_SET_BB_PWR;      name = "air power mW"; break;
            case CAM_FOCUS:     cmd = sky::CMD_SET_CHN_FOCUS;   name = "focus mode";   break;
            default: continue;
        }
        // ar_ldy_gnd 0x89060 builds this one as {cmd 0x23, len 1}: a single
        // byte, unlike most camera settings, which are a u32.
        // Payload width differs per command: ar_ldy_gnd 0x88fd8 writes cmd 0x22
        // as two little-endian bytes (len 2), 0x89060 writes standby as one,
        // and most camera settings are u32.
        std::vector<uint8_t> f;
        if (todo[i].first == CAM_PWR) {
            uint8_t v[2] = { (uint8_t)(todo[i].second & 0xFF),
                             (uint8_t)((todo[i].second >> 8) & 0xFF) };
            f = sky_proto.build(cmd, v, sizeof(v));
        } else if (todo[i].first == CAM_FOCUS) {
            // {u8 chn, u8 enable} - ar_ldy_gnd's own debug line for this
            // command is "GUI_CMD_SET_CHN_FOCUS, chn=%d, en=%d". chn 0 is
            // the FPV stream, the only channel this menu ever controls.
            uint8_t v[2] = { 0, (uint8_t)todo[i].second };
            f = sky_proto.build(cmd, v, sizeof(v));
        } else if (todo[i].first == CAM_STANDBY || todo[i].first == CAM_BW) {
            f = sky_proto.build_u8(cmd, (uint8_t)todo[i].second);
        } else {
            f = sky_proto.build_u32(cmd, (uint32_t)todo[i].second);
        }
        int w = bb_socket_write(ctrl_sockfd, f.data(), (uint32_t)f.size(), 500);
        printf("ar8030: set %s = %d (cmd 0x%02X) -> %d\n", name, todo[i].second, cmd, w);
        if (w > 0 && read_sky_ack(cmd, 1000)) {
            const int v = todo[i].second;
            switch (todo[i].first) {
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
    }
}

// Apply RF changes parked by the menu. These are ioctls on our own radio, so
// they take effect immediately; the return code is logged so an unsupported
// value is visible rather than silently ignored.
void Ar8030Source::apply_pending_rf() {
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
                apply_tx_power(val);
                break;
            }
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
                bb_set_chan_mode_t m; memset(&m, 0, sizeof(m));
                m.auto_mode = (uint8_t)(val ? 1 : 0);
                printf("ar8030: RF channel hop %s -> %d\n", val ? "ON" : "OFF",
                       ar_ioctl(dev, BB_SET_CHAN_MODE, &m, NULL));
                chan_auto = (val != 0);
                break;
            }
            case RF_CHAN: {
                // val < 0 = hand the channel back to AUTO/ACS; otherwise val is
                // the target frequency in kHz and both ends have to move.
                // BB_SET_FREQ is deliberately not used here - stock never calls
                // it, and on its own it moves nothing (section 31).
                if (val < 0) {
                    bb_set_chan_mode_t m; memset(&m, 0, sizeof(m));
                    m.auto_mode = 1;
                    printf("ar8030: RF chan mode AUTO -> %d\n",
                           ar_ioctl(dev, BB_SET_CHAN_MODE, &m, NULL));
                    chan_auto = true;
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

// --debug-probe-rf: find out which RF controls this board actually honours.
// Unlike the mode probe these are all local ioctls on our own radio - they do
// not touch the air unit - so this is safe to run on a live link (it does
// briefly perturb TX power / MCS, and restores stock's settings at the end).
void Ar8030Source::probe_rf_ioctls() {
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
void Ar8030Source::probe_video_modes() {
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

// --ar8030-mode: ask the air unit to switch the FPV channel to a new
// resolution/frame rate. This is GUI_CMD_SET_CHN_RES in the stock app - the
// same 6-byte SET_CHN_RES command its menu sends when you pick a video mode.
void Ar8030Source::apply_video_mode() {
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

// The video bitrate and the link MCS have to be read together to tell a
// link-limited stream from an encoder that is simply targeting a low rate.
// report_link_status() only fires when the link is idle, which is exactly when
// there is no bitrate to compare against - hence this.
// The baseband can measure air<->ground distance by time of flight. It is off
// by default; BB_CFG_DISTC turns it on. `offset` is a close-range calibration
// (the raw td_out reading with the units side by side) subtracted from every
// result - left at 0 here because we have not calibrated this pair, so treat
// the absolute value as uncalibrated until someone measures it at a known range.
void Ar8030Source::enable_ranging() {
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
int Ar8030Source::current_distance() {
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



uint32_t Ar8030Source::link_freq_khz() {
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


int Ar8030Source::chan_index_for_freq(uint32_t freq_khz, uint32_t *freqs, int *n_out) {
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
        if (ci.freq[i] == freq_khz) idx = i;
    }
    if (n_out) *n_out = n;
    return idx;
}

int Ar8030Source::set_rf_channel(uint32_t freq_khz, bool hop_en) {
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
    bb_set_chan_mode_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.auto_mode = 1;
    ar_ioctl(dev, BB_SET_CHAN_MODE, &cm, NULL);
    chan_auto = true;
    return -5;
}

void Ar8030Source::publish_chan_scan() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev || !osd) return;
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

    if (n > 64) n = 64;   // chan_scan_info carries 64 slots
    chan_scan_info sc;
    memset(&sc, 0, sizeof(sc));
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
    osd->set_chan_scan(sc);
}

void Ar8030Source::publish_link_stats() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev || !osd) return;

    artosyn_stats st;
    memset(&st, 0, sizeof(st));
    st.rf_bw_idx = -1;          // 0 is a real gear (1.25 MHz), so -1 = unknown
    auto to_db = [](uint16_t s) { return s ? 10.0f * log10f((float)s / 36.0f) : 0.0f; };

    bb_get_1v1_info_in_t info_in;
    memset(&info_in, 0, sizeof(info_in));
    info_in.frame_num = 64;
    bb_get_1v1_info_out_t info;
    memset(&info, 0, sizeof(info));
    if (ar_ioctl(dev, BB_GET_1V1_INFO, &info_in, &info) == 0) {
        st.snr        = to_db(info.self.snr);
        st.snr_raw    = info.self.snr;
        st.mcs        = info.self.tx_mcs;
        st.power      = info.self.tx_power;
        // The air VTX power we commanded (cmd 0x22), for the OSD to show instead
        // of the ground uplink readback above. tx_power_mw carries the
        // kArPwrLevels encoding (N, or N+1 for auto-capped-at-N).
        st.air_pwr_mw = tx_power_mw;
        st.gains_cur  = info.self.gain_a;
        st.gains_max  = info.self.gain_b;
        st.ldpc_error = info.self.ldpc_num_err_ratio / 10000.0f;
        st.ldpc_total = info.self.ldpc_tlv_err_ratio / 10000.0f;
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
                st.rf_bw_idx = (bw_tx >= 0 && bw_tx < 6) ? bw_tx : -1;
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

    last_link_kbps = st.rx_data_rate_kbps;   // for the periodic stat line
    last_bw_idx    = st.rf_bw_idx;           // live RF bandwidth gear

    if (st.tx_freq > 0) link_freq_mhz = st.tx_freq;

    // Air clock, for the video delay computed in emit_nal(). Cheap, and the
    // pairing with the local monotonic clock has to be as tight as we can
    // manage or the interpolation inherits the round-trip.
    bb_get_ap_time_out_t apt;
    memset(&apt, 0, sizeof(apt));
    if (ar_ioctl(dev, BB_GET_AP_TIME, NULL, &apt) == 0 && apt.timestamp) {
        ap_time_ms  = apt.timestamp;
        ap_local_ms = now_ms();
    }

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
               st.rx_freq, st.tx_freq, ar_bw_label(st.rf_bw_idx),
               ar_bw_label(last_bw_rx_idx));
    }
    osd->update_artosyn_stats(st);
}

int Ar8030Source::current_rx_mcs() {
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

int Ar8030Source::current_link_state() {
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

void Ar8030Source::report_link_status() {
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

bool Ar8030Source::connect_bb() {
    // The whole bring-up is bracketed, not just bb_socket_open. Every one of
    // these SDK calls waits on the daemon, and a run caught in the act stopped
    // inside bb_dev_open - before a socket was ever asked for. Timing one call
    // only catches the parks that happen in that call; timing the sequence
    // catches all of them, and "we started bringing the link up and never
    // finished" is the condition worth acting on anyway.
    BringupGuard bringup_guard;
    bb_host_t* phost = nullptr;
    if (bb_host_connect(&phost, host.c_str(), host_port)) {
        printf("ar8030: bb_host_connect(%s:%d) failed - is /ar8030soc/daemon running?\n",
               host.c_str(), host_port);
        return false;
    }
    bb_host = phost;

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
            printf("ar8030: %s returned %d (attempt %d/2)\n", name, rc, attempt);
            if (*should_stop) return false;
        }
        printf("ar8030: %s never answered - the daemon is not talking\n", name);
        return false;
    };
    if (!bounded_req(BB_INIT_REQ, "bb_init")) return false;
    if (*should_stop) return false;
    if (!bounded_req(BB_START_REQ, "bb_start")) return false;
    if (*should_stop) return false;

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
    au_slices.clear();
    au_open          = false;
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

    if (skip_handshake) printf("ar8030: handshake SKIPPED (--debug-no-handshake)\n");
    else send_air_handshake();

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
    return true;
}

void Ar8030Source::disconnect_bb() {
    au_slices.clear(); au_open = false;
    // --debug-bb-dump's file was opened lazily and never closed; on a forced exit the
    // tail of the capture was whatever had not been flushed.
    if (dump_fp) { fclose((FILE *)dump_fp); dump_fp = nullptr; }
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

void Ar8030Source::update_stats(size_t frame_size) {
    bytes_received += (long long)frame_size;
    uint64_t t = now_ms();
    if (period_start == 0) { period_start = t; return; }
    if (t - period_start >= 1000) {
        period_start = t;
        double video_bw = (double)bytes_received;   // bytes in the last second
        if (osd) {
            osd->update_video_bandwidth(video_bw / 125000.0);  // -> Mbit/s
            osd->log_csv_row();
            osd->signal_render(prof::kWakeLink);
        }
        if (t - last_stat_ms >= 3000) {
            last_stat_ms = t;
            int mcs = current_rx_mcs();
            // BB_CFG_DISTC is refused before the link is up (-2), exactly as
            // BB_SET_BANDWIDTH is (-7). Retry a few times once we are linked.
            int dist_m = current_distance();
            if (osd) osd->set_link_distance(dist_m);
            publish_link_stats();
            // link_kbps is the SDK's own "theoretical throughput for this
            // slot" (BB_GET_MCS.throughput) - i.e. what the radio believes it
            // can carry. Printing it next to the measured video rate is the
            // only way to tell a link limit from an encoder limit, and every
            // bandwidth theory so far has been argued without it.
            // Cached by publish_link_stats(), called just above on this thread.
            printf("ar8030: rx=%llu bytes nals=%llu frames=%llu lost=%llu telemetry_drained=%llu"
                   " msp=%llu rx_mcs=%d bw=%s MHz link=%.2f Mbps video=%.2f Mbps dist=%d"
                   " air+=%.1fms\n",
                   total_bytes, nal_count, (unsigned long long)frames_seen, frames_lost,
                   ctrl_bytes,
                   msp_bytes, mcs, ar_bw_label(last_bw_idx), last_link_kbps / 1000.0,
                   video_bw / 125000.0, dist_m,
                   air_delay_ms < 0.0f ? 0.0f : air_delay_ms);
        }
        bytes_received = 0;
    }
}

void Ar8030Source::update_dvr(std::shared_ptr<std::vector<uint8_t>> frame) {
    if (osd && osd->is_dvr_screen_enabled()) return;
    // Unconditional: DvrRecorder drops the frame when it is not recording, so
    // the rec button can start and stop us without any plumbing here.
    DvrRecorder::instance().feed(frame);
}

// Emit one NAL (payload without the leading start code) to the decoder.
void Ar8030Source::emit_nal(const uint8_t* nal, size_t len) {
    if (len == 0) return;

    // The AR8030 multiplexes side-channel packets (RTP-like, first byte 0x80)
    // into the video stream, and an RF bit-error can corrupt a real NAL header.
    // Either way forbidden_zero_bit==1 means this is NOT a valid H.26x NAL.
    // Folding such bytes into a picture's access unit makes the rkvdec2 hardware
    // hang (task timeout -> reset storm -> blocky). Drop them so only clean
    // slices reach the decoder (ffmpeg and the stock firmware skip them too).
    if (nal[0] & 0x80) {
        nal_count++;
        // DIAGNOSTIC (section 41.5): this is NOT junk - it is the air unit's
        // per-frame metadata header, which we have been discarding. Candidate
        // layout, from correlating captures against known session settings:
        //   [9]      fps        (0x64=100, 0x3C=60)
        //   [14..15] width  LE  (0x0780=1920, 0x0500=1280)
        //   [16..17] height LE  (0x0438=1080, 0x02D0=720)
        //   [18..21] capture timestamp LE u32, microseconds of air uptime
        // If [18..21] really is an air-clock capture stamp, then paired with
        // BB_GET_AP_TIME (air uptime in ms) it gives the true air->ground
        // delay that stock shows and that the baseband SDK does not expose.
        // Log enough to confirm or kill the theory before wiring anything.
        // Validate before trusting: the Annex-B splitter can hit a false start
        // code inside a header and hand us a shifted fragment. A good header
        // has a plausible resolution and a non-zero fps.
        if (len >= 22) {
            unsigned w   = nal[14] | (nal[15] << 8);
            unsigned h   = nal[16] | (nal[17] << 8);
            unsigned fps = nal[9];
            bool sane = (w >= 160 && w <= 4096) && (h >= 120 && h <= 2160) &&
                        (fps > 0 && fps <= 240);
            if (sane) {
                // Frame counter - detect pictures the link dropped. Needs no
                // clock at all, so it works even before AP_TIME is available.
                uint16_t hseq = (uint16_t)(nal[12] | (nal[13] << 8));
                // The header repeats for each slice of a picture, so anything
                // measured per picture has to be gated on the counter changing.
                const bool new_pic = !hseq_valid || hseq != last_hseq;
                if (hseq_valid) {
                    uint16_t gap = (uint16_t)(hseq - last_hseq);
                    if (gap > 1 && gap < 1000) frames_lost += (gap - 1);
                }
                last_hseq = hseq;
                hseq_valid = true;

                // Are we live yet? Compare how far apart two pictures were
                // captured with how far apart they arrived. Draining a backlog
                // those disagree wildly - 16.7ms of capture in under 4ms of
                // wall time; live they converge.
                if (resync_ && new_pic) {
                    uint32_t cap_hdr = (uint32_t)nal[18] | ((uint32_t)nal[19] << 8) |
                                       ((uint32_t)nal[20] << 16) | ((uint32_t)nal[21] << 24);
                    uint64_t arr_us = now_us();
                    if (resync_prev_us_) {
                        uint32_t d_cap = cap_hdr - resync_prev_cap_;
                        uint64_t d_arr = arr_us - resync_prev_us_;
                        // Skip the outage gap itself and any header we misread:
                        // only ordinary frame spacing says anything about pace.
                        if (d_cap > 0 && d_cap < 1000000) {
                            if (d_arr * 10 >= (uint64_t)d_cap * 6) resync_live_++;
                            else                                   resync_live_ = 0;
                        }
                    }
                    resync_prev_us_  = arr_us;
                    resync_prev_cap_ = cap_hdr;
                }

                if (ap_time_ms) {
                    uint32_t cap_us = (uint32_t)nal[18] | ((uint32_t)nal[19] << 8) |
                                      ((uint32_t)nal[20] << 16) | ((uint32_t)nal[21] << 24);
                    // Air-clock "now": last polled air uptime plus local time
                    // elapsed since. Both clocks tick 1:1 (verified), so the
                    // skew is constant offset + true delay.
                    int64_t air_now_us =
                        (int64_t)(ap_time_ms + (now_ms() - ap_local_ms)) * 1000LL;
                    int64_t skew = air_now_us - (int64_t)cap_us;

                    // Re-baseline the floor every 30s so it tracks slow drift
                    // and recovers if a resync moves the offset.
                    uint64_t hnow = now_ms();
                    if (air_skew_reset_ms == 0) air_skew_reset_ms = hnow;
                    if (hnow - air_skew_reset_ms > 30000) {
                        air_skew_reset_ms = hnow;
                        air_skew_floor_us = skew;
                    }
                    if (skew < air_skew_floor_us) air_skew_floor_us = skew;

                    float d_ms = (float)(skew - air_skew_floor_us) / 1000.0f;
                    if (d_ms >= 0.0f && d_ms < 500.0f) {
                        air_delay_ms = (air_delay_ms < 0.0f)
                                           ? d_ms
                                           : 0.9f * air_delay_ms + 0.1f * d_ms;
                        if (osd) osd->set_air_delay(air_delay_ms);
                    }
                }
            }
        }
        return;
    }

    uint8_t nal_type = (codec == VideoCodec::H265) ? ((nal[0] >> 1) & 0x3F)
                                                   : (nal[0] & 0x1F);

    // Not every "NAL" the splitter finds is video. The air unit interleaves
    // small blocks of its own (26-49 bytes, e.g. "01 80 07 38 04 .." carrying
    // 1920x1080, or "0N 00 00 00 64 00" counting up) that follow a start-code
    // pattern, and whose first byte reads as a slice type - roughly one every
    // 36 pictures at 1080p120. Added to a picture, they were shrugged off by
    // MPP and FFmpeg but made the iPhone's decoder fail the web stream within
    // half a second. A header the spec forbids is not a NAL: forbidden bit
    // set, a layer other than the base layer, or temporal id 0.
    if (codec == VideoCodec::H265 && len >= 2 &&
        ((nal[0] & 0x80) || (((nal[0] & 1) << 5) | (nal[1] >> 3)) != 0 || (nal[1] & 7) == 0)) {
        static uint64_t foreign = 0;
        if (++foreign <= 3 || (foreign % 1000) == 0)
            printf("ar8030: skipped a non-video block in the video stream (%zu bytes, "
                   "hdr %02X %02X; %llu so far)\n", len, nal[0], nal[1],
                   (unsigned long long)foreign);
        return;
    }

    // feed_packet_to_decoder expects the Annex-B start code present, so rebuild it.
    static thread_local std::vector<uint8_t> unit;
    unit.clear();
    unit.reserve(len + 4);
    unit.insert(unit.end(), {0x00, 0x00, 0x00, 0x01});
    unit.insert(unit.end(), nal, nal + len);

    bool is_vcl = (codec == VideoCodec::H265) ? (nal_type <= 31) : (nal_type >= 1 && nal_type <= 5);

    // Resync gate. After a reacquire, everything up to the first keyframe of the
    // new link is dropped - it is either the tail of the old stream or inter
    // frames with no reference to decode against.
    //
    // Parameter sets are the exception and pass through: the decoder caches them
    // and prepends them to the next IDR, so dropping them would leave the
    // keyframe we are waiting for undecodable.
    if (resync_) {
        const bool ps = (codec == VideoCodec::H265)
                            ? (nal_type == 32 || nal_type == 33 || nal_type == 34)
                            : (nal_type == 7 || nal_type == 8);
        // The same test the decoder uses for is_key, so what opens the stream
        // here is exactly what it will treat as a keyframe.
        const bool key = (nal_type >= 19 && nal_type <= 21) || nal_type == 5;
        if (!ps) {
            const uint64_t held = now_ms() - resync_start_ms_;
            const bool timed_out = held > kResyncMaxMs;
            const bool caught_up = resync_live_ >= kResyncLive || timed_out;
            if (is_vcl && key && caught_up) {
                resync_ = false;
                printf("ar8030: resync - live after %llums (%s), video resumes\n",
                       (unsigned long long)held,
                       timed_out ? "timed out, showing it anyway" : "arriving at capture rate");
            } else {
                return;                       // still catching up
            }
        }
    }

    uint64_t recv_us = now_us();

    // The air unit slices each picture into ~4 slice NALs. Feeding each slice to
    // MPP as its own packet/PTS corrupts frame + reference assembly (partial
    // "top-only" pictures and an rkvdec timeout/reset storm). Accumulate all the
    // slices of one picture and feed them as a single access unit with one PTS.
    if (vdec && decode_enabled) {
        if (is_vcl) {
            // first_slice_segment_in_pic_flag is the first bit after the HEVC
            // 2-byte NAL header; a 1 marks the start of a new picture.
            uint8_t first_slice = (codec == VideoCodec::H265)
                ? ((len >= 3) ? ((nal[2] >> 7) & 1) : 1)
                : 1;  // H.264 (unused for AR8030): one slice == one picture
            if (first_slice && au_open) {
                flush_access_unit(recv_us);
            }
            if (first_slice) {
                au_pts = frame_pts;
                au_nal_type = nal_type;   // picture key-ness (IDR vs trailing)
                au_first_recv_us = recv_us;  // zero point for this picture
                frames_seen++;            // count real pictures, not slices
            }
            au_slices.insert(au_slices.end(), unit.begin(), unit.end());
            au_last_recv_us = recv_us;   // newest slice of this picture
            au_open = true;
        } else {
            // VPS/SPS/PPS are cached and SEI dropped inside the decoder; the
            // cached param sets get prepended to the next IDR automatically.
            vdec->feed_packet_to_decoder(unit.data(), (int)unit.size(), frame_pts,
                                         recv_us, nal_type, 0, 0);
        }
    }

    const bool is_param_set = (codec == VideoCodec::H265)
                                  ? (nal_type == 32 || nal_type == 33 || nal_type == 34)
                                  : (nal_type == 7 || nal_type == 8);

    // Keep our own copy of the parameter sets for the DVR. The decoder's cache
    // is private to it, and a recording started mid-stream needs VPS/SPS/PPS
    // ahead of its first IDR or the muxer emits an empty hvcC and the file
    // plays black. H.265: VPS 32, SPS 33, PPS 34. H.264: SPS 7, PPS 8.
    //
    // Validate before caching. An RF bit-error can flip a slice NAL into
    // something that *looks* like a parameter set: we saw "41 1F ..." and
    // "44 1F ..." land in hvcC, which decode to nuh_layer_id 35 (the base layer
    // must be 0) and made players report "PPS id out of range" and refuse the
    // file. Type alone is not enough - the rest of the header has to be sane.
    // Exactly one of each type is kept, so hvcC can never accumulate copies.
    if (is_param_set && len >= 6) {
        bool valid;
        int ps_id = 0;
        if (codec == VideoCodec::H265) {
            // 2-byte header: nuh_layer_id must be 0 and nuh_temporal_id_plus1 1,
            // which for a base-layer parameter set means byte 1 is exactly 0x01
            // and the low bit of byte 0 (the top layer_id bit) is clear. Then
            // the contents: an id in range, and for an SPS a real picture.
            valid = ((nal[0] & 0x01) == 0) && (nal[1] == 0x01);
            if (valid) {
                ps_id = hevc_param_set_id(nal_type, nal + 2, len - 2);
                valid = ps_id >= 0;
            }
        } else {
            valid = ((nal[0] & 0x60) != 0);   // nal_ref_idc != 0
        }
        if (valid) {
            // One set per (type, id). This stream uses more than one PPS, so a
            // single set per type left slices unable to resolve theirs
            // ("PPS changed between slices"); keyed by id, every PPS is kept,
            // and a newer set with an id already held - a camera mode change
            // sends a new SPS under the same id - replaces the old one rather
            // than sitting next to it in hvcC.
            auto key_of = [&](const std::vector<uint8_t>& q) {
                // q is Annex-B: 4-byte start code, 2-byte header, payload.
                const int t = (codec == VideoCodec::H265) ? ((q[4] >> 1) & 0x3F) : (q[4] & 0x1F);
                const int id = (codec == VideoCodec::H265)
                                   ? hevc_param_set_id(t, q.data() + 6, q.size() - 6) : 0;
                return std::make_pair(t, id);
            };
            const auto key = std::make_pair((int)nal_type, ps_id);
            bool known = false, changed = false;
            for (auto& q : dvr_param_sets) {
                const bool same = q.size() == unit.size() &&
                                  memcmp(q.data(), unit.data(), unit.size()) == 0;
                if (same) { known = true; break; }
                if (codec == VideoCodec::H265 && key_of(q) == key) {
                    q = unit; known = changed = true; break;
                }
            }
            if (!known) {
                if (dvr_param_sets.size() >= 16) dvr_param_sets.erase(dvr_param_sets.begin());
                dvr_param_sets.push_back(unit);
                changed = true;
            }
            if (changed) {
                DvrRecorder::instance().set_parameter_sets(dvr_param_sets);
                WebStream::instance().set_parameter_sets(dvr_param_sets);
                printf("ar8030: cached param set type=%d id=%d len=%zu (%zu total)\n",
                       nal_type, ps_id, unit.size(), dvr_param_sets.size());
            }
        } else {
            ps_dropped++;
            if (ps_dropped <= 5 || (ps_dropped % 100) == 0)
                printf("ar8030: dropped corrupt param-set NAL type=%d hdr=%02X %02X "
                       "(total %llu)\n", nal_type, nal[0], nal[1],
                       (unsigned long long)ps_dropped);
        }
    }

    // Unconditional: `dvr` is the legacy --dvr shared_ptr and is always null
    // now that recording is driven by the REC button, so guarding on it here
    // meant DvrRecorder never received a single frame - the file was created
    // and the on-screen timer ran while nothing was written. DvrRecorder::feed()
    // drops frames cheaply when idle, so no guard is needed.
    //
    // Parameter sets are deliberately NOT forwarded: DvrRecorder replays the
    // cached ones once when recording starts. The air unit re-sends VPS/SPS/PPS
    // periodically, and feeding minimp4 the same set twice makes it append a
    // second, malformed entry to the hvcC arrays while leaving numNalus stale -
    // ffmpeg then rejects the file with "Invalid NAL unit size in extradata"
    // and QuickTime refuses it outright.
    // VCL NALs are accumulated and handed over as one access unit in
    // flush_access_unit(). Parameter sets are replayed by DvrRecorder at record
    // start. Anything else (SEI and friends) is left out: handed over on its
    // own it became an MP4 sample of its own - an empty "frame" with a frame's
    // duration between every two pictures - and nothing in it is needed to
    // play the file.

    update_stats(unit.size());

    nal_count++;
}

// Feed the accumulated slices of one picture as a single access unit / PTS.
void Ar8030Source::flush_access_unit(uint64_t recv_us) {
    if (!au_open) return;
    if (!au_slices.empty() && vdec) {
        // Hand the decoder this AU's OWN first-slice arrival, not the arrival
        // of the read that happens to be flushing it - see au_first_recv_us.
        const uint64_t au_recv = au_first_recv_us ? au_first_recv_us : recv_us;
        vdec->feed_packet_to_decoder(au_slices.data(), (int)au_slices.size(),
                                     au_pts, au_recv, au_nal_type, 0, 0);
        frame_pts++;

        // Reassembly span: first slice of THIS picture on the wire to its
        // last. Must use au_last_recv_us, not the flush-triggering read -
        // that read is the NEXT picture's first slice, so it would measure
        // the inter-frame interval (~10ms at 100fps) instead.
        //
        // Expect 0.0 much of the time: recv_us is stamped per socket read,
        // and a whole picture usually arrives inside one read, so all its
        // slices share a timestamp. Non-zero means the picture straddled
        // reads - which is exactly the case worth seeing, since that is when
        // waiting for a complete picture actually costs latency.
        if (osd && au_last_recv_us >= au_recv) {
            float rsm_ms = (float)(au_last_recv_us - au_recv) / 1000.0f;
            if (rsm_ms < 200.0f) {   // ignore absurd outliers
                static float rsm_ewma = -1.0f;
                rsm_ewma = (rsm_ewma < 0.0f) ? rsm_ms
                                             : 0.9f * rsm_ewma + 0.1f * rsm_ms;
                osd->set_reassembly_latency(rsm_ewma);
            }
        }
    }
    // The DVR gets the same complete access unit. Feeding it slice by slice
    // made minimp4 emit one MP4 sample per slice, so a player saw a "frame"
    // holding a single slice and dropped the rest with "PPS changed between
    // slices" - decoding only the top quarter of the picture and painting the
    // remainder green. One picture must be one sample.
    //
    // The web viewer gets it too, independently of the DVR's mode (a screen
    // recording skips update_dvr). One copy serves both.
    if (!au_slices.empty()) {
        auto au = std::make_shared<std::vector<uint8_t>>(au_slices);
        update_dvr(au);
        const bool key = (codec == VideoCodec::H265)
                             ? (au_nal_type >= 19 && au_nal_type <= 21)
                             : (au_nal_type == 5);
        WebStream::instance().feed(au, key);
    }
    au_slices.clear();
    au_open = false;
}

// Split an Annex-B byte stream into NALs. Handles NALs straddling reads by
// keeping the tail in `accum` until the next start code arrives.
void Ar8030Source::consume(const uint8_t* data, size_t len) {
    accum.insert(accum.end(), data, data + len);

    if (accum.size() > AR_MAX_ACCUM) {
        printf("ar8030: no start code in %zu bytes - wrong port or non-AnnexB stream?\n",
               accum.size());
        accum.clear();
        return;
    }

    size_t i = 0, n = accum.size();
    size_t nal_start = SIZE_MAX;   // offset of current NAL payload
    size_t last_consumed = 0;

    while (i + 3 < n) {
        bool sc4 = (accum[i] == 0 && accum[i+1] == 0 && accum[i+2] == 0 && accum[i+3] == 1);
        bool sc3 = (accum[i] == 0 && accum[i+1] == 0 && accum[i+2] == 1);
        if (sc4 || sc3) {
            size_t sc_len = sc4 ? 4 : 3;
            if (nal_start != SIZE_MAX) {
                // NAL runs from nal_start up to this start code.
                emit_nal(accum.data() + nal_start, i - nal_start);
                last_consumed = i;
            }
            nal_start = i + sc_len;
            i += sc_len;
            continue;
        }
        i++;
    }

    // Keep the unterminated tail (from the last start code we saw, or the last
    // 3 bytes which might be a partial start code).
    if (nal_start != SIZE_MAX) {
        size_t keep_from = nal_start - (nal_start >= 4 ? 4 : nal_start);
        accum.erase(accum.begin(), accum.begin() + keep_from);
    } else if (accum.size() > 3) {
        accum.erase(accum.begin(), accum.end() - 3);
    }
    (void)last_consumed;
}

void Ar8030Source::run() {
    // stdout is block-buffered when redirected to a file; line-buffer it so
    // the connect/socket diagnostics below actually reach the log.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    SchedulingHelper::set_thread_params_max_realtime("AR8030_RX", 20);
    printf("ar8030: source starting (host=%s:%d slot=%d port=%d) decode=%s\n",
           host.c_str(), host_port, slot, video_port,
           decode_enabled ? "ON" : "off (--ar8030-decode to enable)");

    std::vector<uint8_t> buf(AR_READ_CHUNK);

    while (!*should_stop) {
        bb_watchdog_alive();   // the loop is still going round
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
                if (connect_fails >= kFailsBeforeDaemonRestart &&
                    (connect_fails % kFailsBeforeDaemonRestart) == 0) {
                    printf("ar8030: %d bring-ups failed - restarting the daemon\n",
                           connect_fails);
                    if (system("/etc/init.d/S60ar8030 restart >/dev/null 2>&1") != 0)
                        printf("ar8030: S60ar8030 restart returned non-zero\n");
                    // It backgrounds its own bring-up: loading the driver and
                    // opening the device takes seconds, and asking too early
                    // just burns another failed attempt.
                    for (int i = 0; i < 100 && !*should_stop; i++) usleep(100000);
                }
                for (int i = 0; i < 10 && !*should_stop; i++) usleep(100000);
                continue;
            }
            connect_fails = 0;
        }

        // Keep the OSD repainting even with no video: its render is driven by
        // frame updates, so an unlinked air unit would otherwise leave the
        // last (empty) frame on screen forever. Pipeline does the same at 1Hz.
        drain_control_socket();
        drain_msp_socket();
        publish_chan_scan();

        // A menu mode change parked by the OSD thread.
        int want = pending_mode_index.exchange(-1);
        if (want >= 0 && want < sky::kFpvModeCount) {
            const sky::VideoMode& m = sky::kFpvModes[want];
            mode_w = m.w; mode_h = m.h; mode_fps = m.fps; mode_chn = 0;
            printf("ar8030: menu requested mode %s\n", m.name);
            apply_video_mode();
        }
        apply_pending_settings();
        apply_pending_rf();
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
            if (tl - last_link_poll_ms >= 400) {
                last_link_poll_ms = tl;
                int ls = current_link_state();
                // Publish the state on this timer, not only when a frame
                // arrives. The HUD's status snapshot used to be refreshed
                // solely from update_stats(), which runs on frame arrival - so
                // the moment the air unit went away and the frames stopped, the
                // snapshot froze at "linked" and nothing downstream could ever
                // see the drop. The one condition the HUD most needs to report
                // was the one condition that stopped it being reported.
                //
                // Only the state is refreshed here. Every other figure - SNR,
                // MCS, distance - keeps its last value, which is exactly what
                // IDLE_2 is meant to show: the last reading, held.
                if (ls >= 0 && osd) osd->update_artosyn_link_state(ls);
                if (ls >= 0 && ls != last_link_state) {
                    printf("ar8030: link state %d -> %d (%s)\n", last_link_state, ls,
                           ls == 2 ? "linked" : ls == 1 ? "connecting" : "not linked");
                    if (ls == 2) {
                        // Whatever is still queued belongs to the link that just
                        // ended. Feeding it to the decoder replays history as
                        // fast as it arrives - the picture races, which is worse
                        // than no picture at the moment you are trying to work
                        // out where the aircraft is. Drop it and wait for a
                        // keyframe from the new link.
                        au_slices.clear();
                        au_open = false;
                        resync_          = true;
                        resync_live_     = 0;
                        resync_prev_us_  = 0;
                        resync_prev_cap_ = 0;
                        resync_start_ms_ = now_ms();
                        printf("ar8030: link up - dropping video until it arrives "
                               "at the rate it was captured\n");
                        printf("ar8030: link up - sending the air handshake now that "
                               "there is a link to carry it\n");
                        send_air_handshake();
                        last_rehandshake_ms = tl;
                        rehandshakes = 0;
                    }
                    last_link_state = ls;
                }
            }
        }

        uint64_t now = now_ms();
        if (osd && now - last_tick_ms >= 1000) {
            last_tick_ms = now;
            // Only publish an idle (zero) bitrate once video has actually
            // stopped. update_stats() publishes the true figure once a second
            // while data flows, so zeroing unconditionally here made the OSD
            // alternate between the real bitrate and 0 every other second.
            if (now - last_data_ms >= 1500) {
                osd->update_video_bandwidth(0.0);   // idle OSD tick
                osd->log_csv_row();
                osd->signal_render(prof::kWakeLink);
            }
        }

        // timeout in ms; short enough that *should_stop is honoured promptly.
        int rd = bb_socket_read(sockfd, buf.data(), (uint32_t)buf.size(), 500);
        if (rd > 0) {
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
            consume(buf.data(), (size_t)rd);
        } else {
            // bb_socket_read() returns -1 both for a plain read timeout (no
            // air unit transmitting) and for a dead socket, so a bare -1 is
            // not a reason to tear the link down. Only reconnect once reads
            // have been failing for long enough that the socket is suspect.
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
            uint64_t t_now = now_ms();
            // Tight enough that an air unit coming back on its own power is
            // streaming again inside ten seconds: about three to notice, then
            // the handshake itself.
            bool air_talking = last_ctrl_ms && (t_now - last_ctrl_ms) < 3000;
            bool video_gone  = last_data_ms && (t_now - last_data_ms) > 2500;
            bool cooled      = (t_now - last_rehandshake_ms) > 6000;
            if (air_talking && video_gone && cooled && rehandshakes < 4) {
                last_rehandshake_ms = t_now;
                rehandshakes++;
                printf("ar8030: video silent %llums but telemetry still arriving - "
                       "re-sending the air handshake (%d/4)\n",
                       (unsigned long long)(t_now - last_data_ms), rehandshakes);
                send_air_handshake();
            }
            // NOT while binding. A bind window has no video by definition -
            // no air unit is associated yet, that is the entire point - so
            // this recovery reads the silence as a dead socket and tears down
            // the link the bind is running on. disconnect_bb() closes the
            // device, which loses BB_SET_PAIR_MODE, so any bind outliving
            // this timer was being killed from inside. Seen on hardware as
            // repeated "shutdown:" and "bring-ups failed - restarting the
            // daemon" in the middle of an open window.
            if (read_timeouts >= 200 && !bind_busy()) {
                printf("ar8030: link idle too long, reopening socket\n");
                disconnect_bb();
                accum.clear();
                read_timeouts = 0;
                sleep(1);
            }
        }
    }

    printf("ar8030: source stopped (%llu frames)\n", (unsigned long long)frames_seen);
    disconnect_bb();
}

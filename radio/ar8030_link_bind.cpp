// The AR8030 link's binding: pairing with an air unit, and which one it is
// (its key for its own settings).
#include "radio/ar8030_link_detail.hpp"

#include <fcntl.h>

using namespace radio;

// Parse the paired air-unit MACs out of /factory/user_cfg.json. The file is a
// flat object holding "bb_mac_addr_N": [ "0xE5", "0xDA", "0x00", "0x08" ], the
// same values the stock firmware logged as candi_mac[N].
int ar_load_candidate_macs(bb_mac_t *out, int max) {
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


// ---- binding ---------------------------------------------------------------
//
// Ask the radio to listen for an air unit announcing itself. bind_begin()
// opens the window, bind_step() polls it, bind_finish() closes it and - on
// success - writes the peer into /factory/user_cfg.json so the pairing is
// still there after a reboot.

bool Ar8030Link::bind_begin() {
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
    bind_state.store(gnd::BIND_RUNNING);
    return true;
}

void Ar8030Link::bind_finish(bool ok, const uint8_t peer[4], bool cancelled) {
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
        bind_state.store(gnd::BIND_IDLE);
        bind_done_ms_ = 0;
        report_link_status();
        return;
    }

    if (ok && peer) {
        bind_peer.store(((unsigned)peer[0] << 24) | ((unsigned)peer[1] << 16) |
                        ((unsigned)peer[2] << 8)  |  (unsigned)peer[3]);
        bind_state.store(gnd::BIND_OK);

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
        bind_state.store(gnd::BIND_FAILED);
        printf("ar8030: binding timed out after %ds - no air unit answered.\n"
               "        The air unit only announces itself in ITS pairing mode:\n"
               "        hold its bind button until the LED turns RED, then retry.\n",
               kBindWindowS);
    }
    report_link_status();
}

void Ar8030Link::bind_step() {
    const uint64_t now = now_ms();
    int st = bind_state.load();

    // Let a finished result sit on screen a few seconds, then clear it.
    if ((st == gnd::BIND_OK || st == gnd::BIND_FAILED || st == gnd::BIND_LINKED) && bind_done_ms_ &&
        now - bind_done_ms_ > (uint64_t)kBindResultHoldMs) {
        bind_state.store(gnd::BIND_IDLE);
        bind_done_ms_ = 0;
        st = gnd::BIND_IDLE;
    }

    // A request parked by the menu or the front-panel button.
    if (bind_request.exchange(0) == 1) {
        if (st == gnd::BIND_RUNNING) {
            // Pressing again while a window is open cancels it. With a
            // two-minute window there has to be a way out other than waiting
            // it out or pulling the power.
            printf("ar8030: bind cancelled by a second press\n");
            bind_finish(false, NULL, true);
            return;
        }
        if (!bind_begin()) {
            bind_state.store(gnd::BIND_FAILED);
            bind_done_ms_ = now;
        }
        return;
    }

    if (bind_state.load() != gnd::BIND_RUNNING) return;

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
    bind_secs_left.store(left);

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
bool Ar8030Link::save_paired_mac(const uint8_t mac[4]) {
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
    fchmod(fileno(f), 0600);          // stock ships it owner-only
    // The identity file is the one thing that must never end up truncated, so
    // it is written beside the original and moved over it only once complete.
    bool ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    fclose(f);
    if (!ok || rename(kTmp, kPath) != 0) {
        printf("ar8030: cannot replace %s: %s\n", kPath, strerror(errno));
        unlink(kTmp);
        return false;
    }
    // The move on disk too: its directory synced. Stock sync()s, which here
    // (the radio thread) waited for every file being written - a recording
    // on the card too.
    const int dfd = open("/factory", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    printf("ar8030: saved %02X:%02X:%02X:%02X at ring slot %d (next %d)\n",
           mac[0], mac[1], mac[2], mac[3], save_pos, next_pos);
    return true;
}



// ---- which air unit: the key of its own settings ---------------------------
//
// Keyed by the AP MAC we associate with, so a second paired VTX keeps its own
// settings. The MAC is known from /factory/user_cfg.json before the link is up,
// which is what we need: the SET_CONFIG frames go out during the handshake.
void Ar8030Link::set_sky_key_from_mac(const uint8_t mac[4]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "sky_%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
    sky_cfg_key = buf;
}

// The per-air settings key and the AP MAC follow the unit that answered, read
// from the link status at the handshake (before the camera config goes out).
// The key chosen at start is a guess from the pairing list.
void Ar8030Link::identify_peer() {
    bb_dev_handle_t *dev = (bb_dev_handle_t *)bb_dev;
    if (!dev) return;
    bb_get_status_in_t in;
    memset(&in, 0, sizeof(in));
    in.user_bmp = 0x03FF;
    bb_get_status_out_t st;
    memset(&st, 0, sizeof(st));
    if (ar_ioctl(dev, BB_GET_STATUS, &in, &st) != 0 || st.role != BB_ROLE_DEV) return;
    const uint8_t *p = st.link_status[0].peer_mac.addr;
    const bool blank = (!p[0] && !p[1] && !p[2] && !p[3]) ||
                       (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF);
    printf("ar8030: linked peer %02X:%02X:%02X:%02X (state %u)%s\n", p[0], p[1], p[2], p[3],
           st.link_status[0].state, blank ? " - not reported, settings key kept" : "");
    if (blank || st.link_status[0].state != 2) return;
    // Only a unit this goggle is paired with: Open8030 reports a placeholder
    // (66:00:00:00) there, and adopting it moved every setting to a new key.
    bb_mac_t macs[BB_CONFIG_MAX_SLOT_CANDIDATE];
    const int n = ar_load_candidate_macs(macs, BB_CONFIG_MAX_SLOT_CANDIDATE);
    bool paired = false;
    for (int i = 0; i < n && !paired; i++) paired = memcmp(macs[i].addr, p, BB_MAC_LEN) == 0;
    if (!paired) {
        printf("ar8030: linked peer is not a paired unit - settings key kept\n");
        return;
    }
    char key[32];
    snprintf(key, sizeof(key), "sky_%02X%02X%02X%02X", p[0], p[1], p[2], p[3]);
    settings_.set(radio::setting::ar8030_last_peer, (int)(((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]));
    if (sky_cfg_key == key) return;
    // Another aircraft: its FC screen starts over (belt and braces: its
    // link-up has started a session already).
    if (!sky_cfg_key.empty()) fc_out_.new_session();
    printf("ar8030: settings key %s -> %s\n", sky_cfg_key.c_str(), key);
    sky_cfg_key = key;
    load_sky_link();
    apply_tx_power(tx_power_mw);
    bb_set_ap_mac_t ap;
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.mac.addr, p, BB_MAC_LEN);
    printf("ar8030: BB_SET_AP_MAC(%02X:%02X:%02X:%02X) -> %d\n", p[0], p[1], p[2], p[3],
           ar_ioctl(dev, BB_SET_AP_MAC, &ap, NULL));
}

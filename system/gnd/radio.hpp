#pragma once
// Our radio: the video link as it measures it, the distance, its settings,
// its pairing window, its channel scan.
#include <cstdint>

#include "system/part.hpp"

namespace gnd {

// LinkStats::state when the link is up.
constexpr int kLinkUp = 2;

// The link as the radio measures it. A figure the radio does not give stays
// at its default.
struct LinkStats {
    float snr = 0;
    int snr_raw = 0;
    int mcs = 0;
    int power = 0;              // our radio's own TX power, dBm
    // The air unit's TX power in mW as asked of it, 0 = unknown, N+1 =
    // "auto capped at N" (kArPwrLevels encoding): what the HUD shows as
    // power - the video link's, not our own uplink's.
    int air_pwr_mw = 0;
    int gains_cur = 0;          // receive gain, its step and its top
    int gains_max = 0;
    float fec_error = 0;        // forward error correction: blocks it failed, of those it tried
    float fec_total = 0;
    int role = 0;               // 0: access point, 1: client
    char mac[20] = {0};
    int state = 0;              // kLinkUp: up
    char peer_mac[20] = {0};
    int tx_freq = 0;            // MHz
    int rx_freq = 0;
    int tx_bw = 0;
    int rx_bw = 0;
    // The channel's width as a step: 0 1.25, 1 2.5, 2 5, 3 10, 4 20,
    // 5 40 MHz, -1 unknown (bw_step_label()). A step, not MHz: an int of
    // MHz made 2.5 a "2".
    int bw_step = 0;
    // The radio's TDD frame, us (0: not known), and the frame mode asked of
    // it (2 short, 0 standard).
    float frame_us = 0;
    int frame_mode_asked = 0;
    bool pwr_auto = false;      // TX power adaptation on
    uint32_t data_rate_kbps = 0;      // sending
    int tx_mcs_val = 0;
    uint32_t rx_data_rate_kbps = 0;   // receiving
    int rx_mcs_val = 0;
    bool active = false;
    uint64_t last_update_ms = 0;
    uint32_t ap_timestamp = 0;        // the radio's clock, as it last read it
    uint64_t ap_timestamp_local_ms = 0;
};

// A bandwidth step (LinkStats::bw_step) as the HUD writes it, in MHz.
static inline const char* bw_step_label(int step) {
    static const char* k[] = { "1.25", "2.5", "5", "10", "20", "40" };
    return (step >= 0 && step < 6) ? k[step] : "--";
}

// One sample of the radio's channel scan (the channel-scan screen): each
// channel's centre and the energy the radio heard on it. Higher (less
// negative) is busier.
struct ScanSample {
    int chan_num = 0;           // channels (0: no data yet)
    int auto_mode = 0;          // 1: the radio picks the channel itself, 0: fixed
    int acs_chan = 0;           // the channel it picked when the scan began
    int work_chan = 0;          // the channel it is on now
    int freq_mhz[64] = {0};
    int power_dbm[64] = {0};
    // Whether AUTO may take each where the pilot is, at the width the link
    // runs (the radio's rules: the country, Max Bandwidth).
    uint8_t usable[64] = {0};
    uint64_t last_update_ms = 0;      // 0: never
};

// The pairing window (bind()), as the menu shows it. BIND_LINKED: the window
// was closed because the radio started hearing an air unit on its own;
// BIND_FAILED: it ran out in silence.
enum BindState { BIND_IDLE = 0, BIND_RUNNING, BIND_OK, BIND_FAILED,
                 BIND_LINKED };

// Which RF controls the radio accepted, found out when it starts.
enum RfCap { RF_CAP_POWER = 1, RF_CAP_MCS = 2, RF_CAP_LNA = 4,
             RF_CAP_BW = 8, RF_CAP_CHAN = 16 };

struct RadioState {
    LinkStats link{};
    float video_mbps = 0;      // received video, the last second (0 while none comes)
    uint32_t frames_lost = 0;  // pictures the air unit sent that never came (gaps in its counter), since start
    int dist_raw = -1;         // its ranging reading, metres (-1: none)
    // The distance shown, metres (-1: no reading): the reading itself, the
    // radio's delays already off (Ar8030Link::current_distance).
    int dist_m = -1;
    // Its settings as it holds them.
    int radio_fw = 0;          // its firmware: 0 stock, 1 Open8030
    int max_mcs = -1;
    int frame_mode = 0;
    int max_bw_mhz = 0;
    int steady = 0;
    bool chan_auto = false;    // channel adaptation on
    int chan_hop = 0;          // AUTO HOP (1) or AUTO (0)
    unsigned rf_caps = 0;      // RfCap: the RF controls it accepted
    int live_dbm = 0;          // its TX power as it last said it (Open8030 event 14), dBm; 0: not known
    // Its pairing window.
    int bind_state = 0;        // BindState
    int bind_secs_left = 0;
    unsigned bind_peer = 0;    // peer MAC, packed big-endian
    uint32_t taken = 0;        // the last change carried out (Ticket::seq)
};

class Radio : public Part<RadioState> {
public:
    virtual Ticket set_firmware(int firmware) = 0;    // 0 stock, 1 Open8030: the radio restarts on it
    virtual Ticket set_frame(int mode) = 0;           // 0 standard, 2 short
    virtual Ticket set_max_mcs(int mcs) = 0;          // -1: none
    virtual Ticket set_max_bandwidth(int mhz) = 0;    // 0: no cap
    virtual Ticket set_steady(bool on) = 0;           // RADIO > Steady Latency
    // The channel page. A channel picked: both ends move there, FIXED from
    // then on. AUTO or AUTO HOP: its pick is made now. Search: from a picked
    // channel, the ground searches again.
    virtual Ticket set_channel(int mhz) = 0;
    virtual Ticket set_channel_auto(bool hop) = 0;
    virtual void search_channels() = 0;
    virtual void scan_screen(bool open) = 0;          // the scan polled fast while it is open
    virtual void bind() = 0;                          // the pairing window
    virtual void region_changed() = 0;                // AUTO's channels: the country, licence or time zone
    // What it sends as it comes, every item, each waking the watcher: the
    // channel scan, and the video received each second (Mbit/s).
    virtual bool take_scan(ScanSample& s) = 0;
    virtual bool take_rate(float& mbps) = 0;
    virtual uint64_t dropped() const = 0;             // scans and rates that came faster than taken
};

}  // namespace gnd

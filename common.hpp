#ifndef COMMON_H
#define COMMON_H

#include <cstring>
#include <vector>
#include <stdint.h>
#include <cmath>

enum class VideoCodec {
    UNKNOWN=0,
    H264,
    H265
};

static VideoCodec video_codec(const char * str) {
    if (!strcmp(str, "h264")) {
        return VideoCodec::H264;
    }
    if (!strcmp(str, "h265")) {
        return VideoCodec::H265;
    }
    return VideoCodec::UNKNOWN;
}

typedef struct  {
    uint16_t freq;
    uint8_t bandwidth;
    uint8_t mcs_index;
    int8_t rssi_min;
    int8_t rssi_max;
    int8_t rssi_avg;
    int8_t snr_min;
    int8_t snr_max;
    int8_t snr_avg;
} antenna_stats;

typedef struct  {
    uint32_t count_p_all;
    uint32_t count_p_fec_recovered;
    uint32_t count_p_lost;
    uint32_t count_p_duped;
    uint32_t p_all_rate;
    uint32_t p_fec_recovered_rate;
    uint32_t p_lost_rate;
    uint32_t p_duped_rate;
    uint32_t bandwidth_data_bps;
    uint32_t bandwidth_parity_bps;
    uint8_t fec_k;
    uint8_t fec_n;
    double wfb_fec_rate;
    std::vector<antenna_stats> antennas;
} packets_stats;

typedef struct {
    uint64_t recv_time_us;
    uint32_t tx_capture_delay_us;
    uint32_t tx_processing_delay_us;
    int8_t rssi[1];
    int8_t noise[1];
    uint8_t mcs_index;
    uint8_t bandwidth;
    char if_name[16];
} RadioRxInfo;

typedef struct {
    float snr;
    int snr_raw;
    int mcs;
    int power;                  // ground uplink TX power, dBm (info.self.tx_power)
    // The air VTX's commanded power in mW (what cmd 0x22 asked the air to run),
    // 0 = unknown, N+1 = "auto capped at N" (kArPwrLevels encoding). This is what
    // the OSD shows as "power" - the video-link (air) power the user picked, not
    // the goggle's own uplink, which the ground radio's adaptation can drift.
    int air_pwr_mw;
    int gains_cur;
    int gains_max;
    float ldpc_error;
    float ldpc_total;
    int role; // 0: AP, 1: CP
    char mac[20];
    int state; // 2: CONNECTED, etc.
    char peer_mac[20];
    int tx_freq;
    int rx_freq;
    int tx_bw;
    int rx_bw;
    // Raw bb_bandwidth_e from the radio, -1 = unknown. Stored as the index
    // rather than MHz: the gears are 1.25/2.5/5/10/20/40, so an int-MHz field
    // truncated 2.5 to "2" and 1.25 to "1", which read as a bug on the OSD and
    // hid what the radio was actually reporting. Use ar_bw_label() to display.
    int rf_bw_idx;
    bool pwr_auto;              // TX power adaptation is enabled
    uint32_t data_rate_kbps;    // TX throughput from BB_GET_MCS (kbps)
    int tx_mcs_val;             // TX MCS from BB_GET_MCS
    uint32_t rx_data_rate_kbps; // RX throughput from BB_GET_MCS RX direction (kbps)
    int rx_mcs_val;             // RX MCS from BB_GET_MCS RX direction
    bool active;
    uint64_t last_update_ms;
    uint32_t ap_timestamp;
    uint64_t ap_timestamp_local_ms;
} artosyn_stats;

// TX power levels, in mW, with the dBm the radio actually wants.
//
// Recovered from ar_ldy_gnd's do_bb_set_local_power (table at 0x30b250, chosen
// when the --subtype global reads 2, i.e. this board - the same guard as the
// PRJ_DISPATCH call). Stock's encoding is reused verbatim: a value of N mW
// means "hold N", and N+1 means "auto, capped at N" - which is where its menu
// strings "500mW Auto" and "1W Auto" come from.
//
// The offered set is stock's, not the whole mW table. GlassesUI holds six
// per-product lists as one 33-pointer array at 0x6f15d0:
//
//   5  25mW, 100mW, 200mW, 500mW, 500mW Auto          <- this board
//   3  25mW, 100mW, 100mW Auto
//   1  25mW
//   8  25mW, 100mW, 200mW, 500mW, 1W, 1W Auto, 2W, 2W Auto
//  14  ... up to 10W / 10W Auto
//   2  25mW, 100mW
//
// The array is indexed at runtime with no static xref to the group bases, so
// the list is identified by evidence rather than by the selector: stock's own
// menu on this unit reads "500mW Auto", and that string occurs in exactly one
// of the six lists. The wider lists belong to other products in the family -
// and offering them here would be wrong twice over, since bb_set_pwr_in_t
// documents pwr as [0-31 dBm] and section 11 has this board browning out
// under load.
struct ar_pwr_level { int mw; int dbm; bool automode; const char* label; };
// The mw field doubles as the value sent to the air unit (cmd 0x22), where
// stock's convention is that AUTO is the level plus one - a bbproxy capture of
// stock caught 101 on the wire for its "100mW Auto" entry, so 501 here means
// "auto, capped at 500 mW".
//
// Stock does not have one fixed list: GlassesUI picks between several at run
// time from `sky_prj_name`, which the *air unit* reports in RSV_SKY_CAM_CFG
// (getter at 0x42068, set at 0x86ab0, compared against 4/6/7 in the menu
// builder). The alternatives include a 100mW-Auto-capped list and one that goes
// to 2W. This board pairs with an Ascent Lite+, which is 500 mW, so that is the
// list offered here - see references/sky-commands.md.
static const ar_pwr_level kArPwrLevels[] = {
    {   25, 11, false, "25mW"      }, {  100, 17, false, "100mW"     },
    {  200, 20, false, "200mW"     }, {  500, 24, false, "500mW"     },
    {  501, 24, true,  "500mW AUTO"},
};
static const int kArPwrCount = (int)(sizeof(kArPwrLevels) / sizeof(kArPwrLevels[0]));

static inline int ar_pwr_index(int mw) {
    for (int i = 0; i < kArPwrCount; i++) if (kArPwrLevels[i].mw == mw) return i;
    return 3;   // 500mW, what stock boots with and what we used before
}

// The radio reports its current power in dBm; convert to mW for display.
//
// An exact table row wins, so the anchors read as the round numbers the menu
// uses. Everything else is computed - stock's table is log-linear on
// 25 mW @ 11 dBm and reproduces every anchor to within a milliwatt
// (17 -> 99.5, 20 -> 198.6, 24 -> 498.8), so the curve between them is the
// table's own.
//
// This matters for AUTO: the range handed to the radio is 11..24 dBm, and the
// levels it actually sits at in between (12-16, 18, 19, 21-23) have no table
// row. Matching anchors only would mean falling back to dBm exactly when AUTO
// is doing its job.
static inline int ar_pwr_mw_from_dbm(int dbm) {
    for (int i = 0; i < kArPwrCount; i++)
        if (!kArPwrLevels[i].automode && kArPwrLevels[i].dbm == dbm)
            return kArPwrLevels[i].mw;
    if (dbm <= 0) return 0;
    return (int)(25.0 * pow(10.0, (dbm - 11) / 10.0) + 0.5);
}

// bb_bandwidth_e gear -> display label. Not a number, because two of the six
// gears are fractional MHz.
static inline const char* ar_bw_label(int idx) {
    static const char* k[] = { "1.25", "2.5", "5", "10", "20", "40" };
    return (idx >= 0 && idx < 6) ? k[idx] : "--";
}

// Per-channel spectrum scan (BB_GET_CHAN_INFO): each channel's center frequency
// and the radio's periodic-scan averaged energy. Feeds the OSD channel-scan
// screen so the user can see how busy each channel is. A higher (less negative)
// power_dbm means a busier channel (e.g. an overlapping wifi AP).
typedef struct {
    int      chan_num;          // number of valid channels (0 = no data yet)
    int      auto_mode;         // 1=auto hop (ACS), 0=manual
    int      acs_chan;          // index ACS selected at scan start
    int      work_chan;         // index the radio is currently receiving on
    // 64, not 32: this radio reports chan_num=42 with work_chan=36, so a
    // 32-entry table silently dropped the upper channels AND put the current
    // channel out of range, which is why it was never highlighted.
    int      freq_mhz[64];      // per-channel center frequency (MHz)
    int      power_dbm[64];     // per-channel averaged scan energy (dBm)
    uint64_t last_update_ms;    // 0 = never populated
} chan_scan_info;

// Air-side adaptation decisions, received over the RF status channel
// (tx_send_status 0x30-0x35). Lets the gnd log/overlay what bw_adapt and the
// TX water-level are doing, with no management network to the air.
typedef struct {
    uint32_t applied_bitrate_kbps;  // encoder target the air applied
    uint32_t link_throughput_kbps;  // link capacity the air adapted to
    int      tier;                  // bw_adapt tier index
    uint32_t wl_drops;              // water-level cumulative dropped frames
    uint32_t wl_latency_ms;         // water-level modeled in-flight latency
    uint32_t fps;                   // encoder frame rate the air applied
    uint64_t last_update_ms;
} adapt_stats;

#endif
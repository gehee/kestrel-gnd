#pragma once
// What the AR8030 link runs with: the settings file's values and the command
// line's, read by main() and given to the link when it is made
// (Ar8030Link's constructor). After that, the link's values are its own -
// changed by its commands and its threads, nothing else.
#include <cstdlib>
#include <string>

#include "system/air/power.hpp"

struct Ar8030Config {
    // The channel and width to start on (--ar8030-freq / --ar8030-bw).
    // Defaults taken from the stock boot log for this board (rf_board 16):
    //   gui_fpv_bb_set_freq, freq=5740000 ; gui_fpv_bb_set_bandwidth, bandwidth=0
    unsigned freq_khz = 5740000;
    // -1 = leave the radio on whatever bandwidth it negotiates.
    //
    // Forcing this from the ground does NOT work and measurably hurts: the air
    // unit is the AP and owns the channel, so a unilateral BB_SET_BANDWIDTH(20M)
    // on the DEV side is accepted (returns 0) and then costs throughput -
    // measured 3.06 -> 2.10 Mbps at the USB layer, MCS 10 -> 8.
    int bandwidth = -1;
    bool do_pair = false;
    int ap_index = 0;                // which bb_mac_addr_N to use as the AP
    bool chan_auto = true;           // channel adaptation (scan for the AP)
    bool chan_manual_cli = false;    // --ar8030-chan-manual: pinned to freq_khz from the start

    // TX power: stock's encoding (N = hold N mW, N+1 = auto capped at N), as
    // chosen, and our radio's own dBm.
    int tx_power_dbm = kArPwrLevels[ar_pwr_index(kArPwrDefaultMw)].dbm;
    int tx_power_mw = kArPwrDefaultMw;
    int tx_power_want_mw = kArPwrDefaultMw;

    // The fastest a picture's first slice gets from its capture stamp to our
    // read: the one calibration the air delay needs, since our clock and the
    // air unit's are not synced (air_floor_ms). Measured with both clocks
    // synced to a host over USB, on the stock air firmware: 17.4-17.9 ms on
    // 2026-09-29 (1080p100 and 720p100), 20.1-20.7 ms on 2026-09-30 (1080p100,
    // three thousand pictures a run) - it moves with the link, so the figure
    // is only as good as this number for the setup at hand.
    int air_floor_us = 20500;

    bool skip_handshake = false;
    int standby_mode = -1;           // -1 leaves the air unit's standby alone, 0/1 pushed after every handshake
    int prj_rf_bw = -1;              // BB_SET_PRJ_DISPATCH cmd 138 (stock: 4); -1 on either disables it
    int prj_rf_pwr_mw = -1;          // (stock: 500)
    int air_bw = -1;                 // air-side bandwidth, sky cmd 0x24, UNVERIFIED units; -1 disables
    int max_kbps = 0;                // ar8030_max_kbps: the video bitrate cap sent at link-up, 0 = none
    bool air_logs = false;           // hud_debug: the air unit's flight logs, sent at link-up
    // Pictures decoded from their first slice (Vdec::stream_supported): on
    // unless KESTREL_STREAM_DECODE=0 or stream_decode 0.
    bool stream_decode = !(getenv("KESTREL_STREAM_DECODE") && atoi(getenv("KESTREL_STREAM_DECODE")) == 0);
    int max_bw_mhz = 40;             // ar8030_max_bw_mhz: 5/10/20/40, 0 AUTO (stock: 20 or 40)
    int max_mcs = -1;                // ar8030_max_mcs: Open8030 here, -1 none
    int steady = 0;                  // ar8030_steady: Open8030 here
    int radio_fw = 0;                // ar8030_firmware: 0 stock, 1 Open8030 (bb_only.sh loads it)
    int frame_mode = 2;              // ar8030_frame_mode: 2 short, 0 standard
    int replay_stock_rf = 1;         // replay stock's startup SET sequence verbatim
    // BB socket port carrying the flight controller's MSP stream, or 0 to
    // leave it alone (2: it rides the telemetry socket).
    int msp_bb_port = 2;
    // --debug-bb-dump: a raw copy of the port-2 telemetry stream (configure +
    // RX only, never TX); --debug-replay: video from a file instead of the
    // baseband; --debug-video-dump: the video socket's bytes to a file.
    std::string dump_path, replay_path, video_dump_path;
    bool decode_enabled = false;     // --ar8030-decode; off = link/OSD only
    // --ar8030-mode WxH@FPS: retune the camera after the handshake (0: what
    // the stock handshake configured).
    int mode_w = 0, mode_h = 0, mode_fps = 0, mode_chn = 0;
    bool probe_modes = false;        // --debug-probe-modes
    bool probe_rf = false;           // --debug-probe-rf
};

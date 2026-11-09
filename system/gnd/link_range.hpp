#pragma once
// The video link's range left - what the canopy's VIDEO track shows - and the
// warning off it (the screen-edge glow), for every frontend: the VRX Pro's OSD
// and the native Ascent's both read them from here, so the two never disagree.
//
// Range left is what the link can still give up to hold on as the signal
// fades, in dB of SNR, summed over its three levers (the user, 2026-10-29: the
// gauge and its warning are about what is left to gain range, not the MCS):
//   MCS   - the current rung's down threshold over the lowest's (the radio's
//           own rows, SET 36: down at -0.9 2.6 5.2 8.3 11.2 14.2 16.1 dB for
//           MCS -1 0 3 5 6 8 10)
//   width - 3 dB a halving left, down to 5 MHz (the narrowest used)
//   power - the air's cap over its power now (AUTO; a fixed power has none
//           to give)
// Full is 40 MHz MCS 8 with the AUTO cap's dB over 22 (where the model sits
// close by) - 28 dB on 500 mW AUTO. The VIDEO track shows it in twelve blocks
// (~2.3 dB each): amber at 5-7, red at 4 or fewer - 20 MHz MCS 6 at full
// power is 18 dB, 8 blocks, still fine; 20 MHz MCS 5 and 10 MHz MCS 5
// (5.9 Mbit/s) are amber; 10 MHz MCS 3 (3.9 Mbit/s, 9 dB, the user: should be
// red) and 5 MHz under MCS 6 are red.
#include <algorithm>

#include "system/air/power.hpp"

namespace gnd {

struct LinkRange {
    float norm = 0.0f;   // of full, 0..1
    int blocks = -1;     // of twelve; -1: no MCS reading, no verdict
    // 0 none, 1 amber, 2 red.
    int warning() const { return blocks < 0 ? 0 : blocks <= 4 ? 2 : blocks <= 7 ? 1 : 0; }
};

// rx_mcs: the radio's MCS index (LinkStats::rx_mcs_val, ar_mcs_label's input);
// bw_step: LinkStats::bw_step (2 = 5 MHz .. 5 = 40 MHz); air_pwr_mw: the air
// unit's level as kArPwrLevels has it (AUTO as N+1), 0 if not known;
// air_live_dbm: its power now (air::RadioState::live_dbm), 0 if not known.
inline LinkRange link_range(bool connected, int rx_mcs, int bw_step, int air_pwr_mw, int air_live_dbm) {
    static const struct { int mcs; float down_db; } kRows[] = {
        { -1, -0.9f }, { 0, 2.6f }, { 3, 5.2f }, { 5, 8.3f },
        { 6, 11.2f }, { 8, 14.2f }, { 10, 16.1f } };
    const int mcs = ar_mcs_label(rx_mcs);
    float mcs_db = -1.0f;
    for (const auto &r : kRows) if (mcs >= r.mcs) mcs_db = r.down_db - kRows[0].down_db;
    const float width_db = bw_step > 2 && bw_step < 6 ? 3.0f * (bw_step - 2) : 0.0f;
    float power_db = 0.0f, power_full_db = 0.0f;
    if (air_pwr_mw > 0) {
        const ar_pwr_level &lv = kArPwrLevels[ar_pwr_index(air_pwr_mw)];
        if (lv.automode) {
            int cap_dbm = 0;   // the air's dBm for the AUTO cap, on its own scale
            for (int d = 9; d <= 28; d++) if (ar_air_mw_from_dbm(d) <= lv.mw - 1) cap_dbm = d;
            power_db = air_live_dbm > 0 && cap_dbm > air_live_dbm ? (float)(cap_dbm - air_live_dbm) : 0.0f;
            power_full_db = cap_dbm > 22 ? (float)(cap_dbm - 22) : 0.0f;
        }
    }
    LinkRange r;
    if (connected && mcs_db >= 0.0f) {
        const float full = (14.2f - kRows[0].down_db) + 9.0f + power_full_db;
        r.norm = std::max(0.0f, std::min(1.0f, (mcs_db + width_db + power_db) / full));
        r.blocks = (int)(r.norm * 12.0f + 0.5f);
    }
    return r;
}

}  // namespace gnd

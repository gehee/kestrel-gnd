#pragma once
// The radio's settings (see Setting, base/settings.hpp). main() reads them
// into the Ar8030Config at start; the changes asked of the parts are saved by
// the model as they are asked (radio::Model::save).

#include <string>

#include "base/settings.hpp"
#include "system/air/power.hpp"

namespace radio::setting {

// RADIO: AUTO HOP; the caps on the video's bitrate (kbps, 0 none), bandwidth
// (MHz) and MCS (-1 AUTO); Steady.
inline constexpr Setting<int> ar8030_chan_hop{"ar8030_chan_hop", 0};
inline constexpr Setting<int> ar8030_max_kbps{"ar8030_max_kbps", 0};
inline constexpr Setting<int> ar8030_max_bw_mhz{"ar8030_max_bw_mhz", 40};
inline constexpr Setting<int> ar8030_max_mcs{"ar8030_max_mcs", -1};
inline constexpr Setting<int> ar8030_steady{"ar8030_steady", 0};
// The VRX's radio firmware (1 Open8030 where it is on the unit, 0 stock) and
// its TDD frame (2 short, 0 standard): bb_only.sh loads them with the radio.
inline constexpr Setting<int> ar8030_firmware{"ar8030_firmware", 1};
inline constexpr Setting<int> ar8030_frame_mode{"ar8030_frame_mode", 2};
// TX power, mW in stock's encoding (N hold, N+1 auto capped at N).
inline constexpr Setting<int> tx_power_mw{"tx_power_mw", kArPwrDefaultMw};
// The last air unit linked to (its radio MAC, as a number).
inline constexpr Setting<int> ar8030_last_peer{"ar8030_last_peer", 0};
// The bring-up: decode on, the baseband's slot and port, its bandwidth gear
// (-1 the radio's own), stock's PRJ_DISPATCH values (-1 off), the fastest
// capture-to-arrival the air delay is pinned to (ms), the MSP port to drain
// (0 off), stock's RF config on replay, the bring-up watchdog.
inline constexpr Setting<int> ar8030_decode{"ar8030_decode", 1};
inline constexpr Setting<int> ar8030_slot{"ar8030_slot", 0};
inline constexpr Setting<int> ar8030_port{"ar8030_port", 3};
inline constexpr Setting<int> ar8030_bw{"ar8030_bw", -1};
inline constexpr Setting<int> prj_rf_bw{"prj_rf_bw", -1};
inline constexpr Setting<int> prj_rf_pwr_mw{"prj_rf_pwr_mw", -1};
inline constexpr Setting<int> air_bw{"air_bw", -1};
inline constexpr Setting<float> air_floor_ms{"air_floor_ms", 20.5f};
inline constexpr Setting<int> msp_bb_port{"msp_bb_port", 2};
inline constexpr Setting<int> replay_stock_rf{"replay_stock_rf", 1};
inline constexpr Setting<int> bb_watchdog{"bb_watchdog", 0};
// Pictures to the decoder slice by slice (0: whole).
inline constexpr Setting<int> stream_decode{"stream_decode", 1};

// An air unit's own, under the key its MAC names (sky_<MAC>): each of its
// camera's settings as it last reported them (def: what it says now), the TX
// power chosen for it, and what it is (its features, 0x100 set once known).
inline MadeSetting<int> air_camera(const std::string& air, const char* what, int def = 0) {
    return {air + "_" + what, def};
}
inline MadeSetting<int> air_tx_power_mw(const std::string& air, int def) {
    return {air + "_tx_power_mw", def};
}
inline MadeSetting<int> air_features(const std::string& air) { return {air + "_air", 0}; }
// HUD > Calib Distance: the ranging reading subtracted from it. Stock
// subtracts a per-index calibration from the raw ranging result and clamps at
// zero (ar_ldy_gnd 0x9be20). Its table, built in that function's prologue, is
// {0, 0, 115, 90, 75, 0, 0, 0} indexed by a byte at ctx+1212 - the offsets
// fall as the index rises, consistent with an RF-bandwidth index (wider
// bandwidth = finer time-of-flight resolution = smaller base). This board
// measures 109-111 with the units together, i.e. stock's index-2 entry, so
// 115 is the default. Recalibrate if the RF bandwidth changes, since the
// correct offset changes with it. Per air unit (by its radio MAC, no colons);
// the plain one is the start for one never calibrated.
inline constexpr Setting<int> dist_offset{"dist_offset", 115};
inline MadeSetting<int> air_dist_offset(const std::string& mac, int def) { return {"dist_offset_" + mac, def}; }

}  // namespace radio::setting

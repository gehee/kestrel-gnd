#pragma once
// The radio's settings (see Setting, base/settings.hpp). main() reads them
// into the Ar8030Config at start; the changes asked of the parts are saved by
// the model as they are asked (radio::Model::save).

#include <string>

#include "base/settings.hpp"
#include "system/air/power.hpp"

namespace radio::setting {

// RADIO: the channel (FIXED, MHz; 0 for none) and AUTO HOP; the caps on the
// video's bitrate (kbps, 0 none), bandwidth (MHz) and MCS (-1 AUTO); Steady.
inline constexpr Setting<int> ar8030_chan_mhz{"ar8030_chan_mhz", 0};
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
// What it is (its project number, system/air/power.hpp): its power levels, before it has said so
// on a link that was lost and reopened.
inline MadeSetting<int> air_prj(const std::string& air) { return {air + "_prj", 0}; }
// The distance's zero for an air unit (by its radio MAC, no colons), as
// "<power-up id>:<raw>".

}  // namespace radio::setting

#pragma once
// The radio's state as the HUD and menu show it: everything they read from
// Ar8030Source, in one value. The drawing code reads the OSD's per-frame copy
// (taken once at the start of each frame); key handling and the menu's sync
// take their own at the moment they act, as they always read it fresh.

#include <string>
#include <vector>

#include "radio/ar8030_source.hpp"

namespace hud {

struct RadioState {
    int air_prj = 0;               // the air unit's power-level set (0: not known yet)
    int radio_fw = 0;              // our radio's firmware: 0 stock, 1 Open8030
    int tx_power_mw = 0;           // TX Power as held (stock's encoding)
    int tx_power_want_mw = 0;      // TX Power as chosen
    int max_mcs = -1;
    int frame_mode = 0;
    int max_kbps = 0;
    int max_bw_mhz = 0;
    int steady = 0;
    bool chan_auto = false;        // channel adaptation on
    int chan_hop = 0;              // AUTO HOP (1) or AUTO (0)
    int bind_state = 0;            // Ar8030Source::BindState
    int bind_secs_left = 0;
    unsigned bind_peer = 0;        // peer MAC, packed big-endian
    Ar8030Source::AirInfo air;     // what the air unit said about itself
};

RadioState read_radio();

// Per-channel and per-air-unit questions, which need the radio's tables
// rather than a copy of them.
inline bool channel_usable(int mhz) { return Ar8030Source::region_usable(mhz); }
inline std::vector<std::string> air_cap_names(const Ar8030Source::AirInfo &a) {
    return Ar8030Source::air_cap_names(a);
}

}  // namespace hud

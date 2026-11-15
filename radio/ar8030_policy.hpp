#pragma once
// Apply the same persisted link policy in both receiver launchers.
#include "radio/ar8030_config.hpp"
#include "radio/settings.hpp"

inline void ar8030_load_policy(Ar8030Config& cfg, Settings& settings, int default_bw_mhz = 40) {
    const int bw = settings.get(Setting<int>{radio::setting::ar8030_max_bw_mhz.key, default_bw_mhz});
    cfg.max_bw_mhz = bw == 0 || bw == 5 || bw == 10 || bw == 20 || bw == 40 ? bw : default_bw_mhz;
    const int mcs = settings.get(radio::setting::ar8030_max_mcs);
    cfg.max_mcs = mcs >= 0 && mcs <= 11 ? mcs : -1;
    cfg.steady = settings.get(radio::setting::ar8030_steady) == 1 ? 1 : 0;
    cfg.chan_hop = settings.get(radio::setting::ar8030_chan_hop) == 1 ? 1 : 0;
    const int fixed = settings.get(radio::setting::ar8030_chan_mhz);
    cfg.chan_fixed_mhz = fixed >= 2000 && fixed <= 7000 ? fixed : 0;
}

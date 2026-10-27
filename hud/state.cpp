#include "hud/state.hpp"

namespace hud {

RadioState read_radio() {
    RadioState s;
    s.air_prj = Ar8030Source::air_prj.load();
    s.radio_fw = Ar8030Source::radio_fw;
    s.tx_power_mw = Ar8030Source::tx_power_mw;
    s.tx_power_want_mw = Ar8030Source::tx_power_want_mw;
    s.max_mcs = Ar8030Source::max_mcs;
    s.frame_mode = Ar8030Source::frame_mode;
    s.max_kbps = Ar8030Source::max_kbps;
    s.max_bw_mhz = Ar8030Source::max_bw_mhz;
    s.steady = Ar8030Source::steady;
    s.chan_auto = Ar8030Source::chan_auto;
    s.chan_hop = Ar8030Source::chan_hop.load();
    s.bind_state = Ar8030Source::bind_state.load();
    s.bind_secs_left = Ar8030Source::bind_secs_left.load();
    s.bind_peer = Ar8030Source::bind_peer.load();
    s.air = Ar8030Source::air_info();
    return s;
}

}  // namespace hud

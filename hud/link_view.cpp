#include "hud/link_view.hpp"

namespace hud {

LinkView link_view(const gnd::RadioState& r, const air::RadioState& a, const air::EncoderState& e,
                   const Asked& asked) {
    LinkView v;
    v.radio_fw = shown(asked.fw, r.radio_fw, r.taken);
    v.max_mcs = shown(asked.mcs, r.max_mcs, r.taken);
    v.frame_mode = shown(asked.frame, r.frame_mode, r.taken);
    v.max_bw_mhz = shown(asked.bw, r.max_bw_mhz, r.taken);
    v.steady = shown(asked.steady, r.steady, r.taken);
    v.chan_auto = r.chan_auto;
    v.bind_state = r.bind_state;
    v.bind_secs_left = r.bind_secs_left;
    v.bind_peer = r.bind_peer;
    v.tx_power_mw = a.tx_power_mw;
    v.tx_power_want_mw = a.tx_power_want_mw;
    v.max_kbps = shown(asked.kbps, e.max_kbps, e.taken);
    return v;
}

}  // namespace hud

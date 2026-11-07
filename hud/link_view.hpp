#pragma once
// The link's settings as the menu and the screens show them: what our radio,
// the air unit's radio and its encoder hold, with what the menu asked of them
// on top until each has carried it out (shown(), system/part.hpp) - so the
// menu reads back what it set at once, while the radio thread may be in a
// bring-up attempt or a 500 ms read for a while.
#include "system/air/encoder.hpp"
#include "system/air/radio.hpp"
#include "system/gnd/radio.hpp"

namespace hud {

// What the menu asked, each with its ticket (Ticket::seq 0: nothing asked).
struct Asked {
    Ticket kbps, bw, frame, mcs, steady, fw, hop;
};

struct LinkView {
    int radio_fw = 0;              // our radio's firmware: 0 stock, 1 Open8030
    int tx_power_mw = 0;           // the air unit's TX Power as it runs (stock's encoding)
    int tx_power_want_mw = 0;      // ... as chosen
    int max_mcs = -1;
    int frame_mode = 0;
    int max_kbps = 0;
    int max_bw_mhz = 0;
    int steady = 0;
    bool chan_auto = false;        // channel adaptation on
    int chan_hop = 0;              // AUTO HOP (1) or AUTO (0)
    int bind_state = 0;            // gnd::BindState
    int bind_secs_left = 0;
    unsigned bind_peer = 0;        // peer MAC, packed big-endian
    bool resend_known = false;     // the air radio's resends are known
};

LinkView link_view(const gnd::RadioState& radio, const air::RadioState& air_radio,
                   const air::EncoderState& encoder, const Asked& asked);

}  // namespace hud

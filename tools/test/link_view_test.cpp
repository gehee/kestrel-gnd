// Unit test of hud/link_view.cpp: what the menu shows of the link's settings -
// the value it asked until the part that holds it has carried the change out
// (its taken at or past the ticket), then the part's own; each setting judged
// by its own part's taken; nothing asked, the parts' values as they are.
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <cstdio>

#include "hud/link_view.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
    gnd::RadioState r;
    air::RadioState a;
    air::EncoderState e;
    hud::Asked asked;

    // Nothing asked: what the parts hold.
    r.radio_fw = 1;
    r.max_mcs = 9;
    r.chan_auto = true;
    r.bind_state = gnd::BIND_RUNNING;
    r.bind_secs_left = 12;
    a.tx_power_mw = 501;
    a.tx_power_want_mw = 200;
    e.max_kbps = 8000;
    hud::LinkView v = hud::link_view(r, a, e, asked);
    CHECK(v.radio_fw == 1 && v.max_mcs == 9 && v.chan_auto);
    CHECK(v.bind_state == gnd::BIND_RUNNING && v.bind_secs_left == 12);
    CHECK(v.tx_power_mw == 501 && v.tx_power_want_mw == 200 && v.max_kbps == 8000);

    // Asked, not carried out yet: the asked values.
    r.taken = 4;
    e.taken = 4;
    asked.mcs = Ticket{5, 3};
    asked.kbps = Ticket{6, 12000};
    asked.frame = Ticket{8, 2};
    v = hud::link_view(r, a, e, asked);
    CHECK(v.max_mcs == 3 && v.max_kbps == 12000 && v.frame_mode == 2);
    CHECK(v.radio_fw == 1 && v.steady == 0 && v.max_bw_mhz == 0);     // not asked: held

    // Our radio has taken them, the encoder not yet: each by its own part.
    r.taken = 8;
    r.max_mcs = 3;
    r.frame_mode = 2;
    v = hud::link_view(r, a, e, asked);
    CHECK(v.max_mcs == 3 && v.frame_mode == 2 && v.max_kbps == 12000);

    // The radio ended elsewhere than asked (a stock radio's width, a refused
    // MCS): once taken, its value, not the one asked.
    r.max_mcs = -1;
    e.taken = 6;
    e.max_kbps = 10000;
    v = hud::link_view(r, a, e, asked);
    CHECK(v.max_mcs == -1 && v.max_kbps == 10000);

    // shown() on its own (system/part.hpp): seq 0 is nothing asked.
    CHECK(shown(Ticket{}, 5, 0) == 5 && shown(Ticket{1, 9}, 5, 0) == 9 && shown(Ticket{1, 9}, 5, 1) == 5);

    std::printf(fails ? "link_view_test: %d FAILED\n" : "link_view_test: all passed\n", fails);
    return fails ? 1 : 0;
}

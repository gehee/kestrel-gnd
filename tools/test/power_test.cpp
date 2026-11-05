// Unit test of system/air/power.hpp: the TX power levels, what an air unit is
// offered, and the menu's two rows (FIXED / AUTO, and the level) over the one
// value on the wire (stock's N, or N + 1 for AUTO up to N).
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <cstdio>
#include <initializer_list>

#include "system/air/power.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
    // AUTO is the level + 1, with the level's own dBm, for every level.
    for (const auto &l : kArPwrLevels) {
        if (!l.automode) continue;
        CHECK(ar_pwr_top(l.mw) == l.mw - 1);
        CHECK(kArPwrLevels[ar_pwr_index(l.mw - 1)].dbm == l.dbm);
        CHECK(!kArPwrLevels[ar_pwr_index(l.mw - 1)].automode);
    }
    CHECK(ar_pwr_is_auto(317) && ar_pwr_top(317) == 316);
    CHECK(!ar_pwr_is_auto(316) && ar_pwr_top(316) == 316);

    // What an air unit is offered: stock's lists; kestrel-air's own only on a
    // Lite+ that has said it takes them.
    CHECK(ar_pwr_offer_index(AR_AIR_LITE_PLUS, 317) < 0);
    CHECK(ar_pwr_offer_index(AR_AIR_LITE_PLUS, 317, true) >= 0);
    CHECK(ar_pwr_offer_index(AR_AIR_LITE, 317, true) < 0);   // a Lite has 100mW AUTO at most
    CHECK(ar_pwr_fit(AR_AIR_LITE_PLUS, 317, true) == 317);
    CHECK(ar_pwr_fit(AR_AIR_LITE_PLUS, 317) == 200);         // stock's list: fixed, its highest under
    CHECK(ar_pwr_fit(AR_AIR_LITE_PLUS, 501) == 501);
    CHECK(ar_pwr_fit(AR_AIR_LITE, 317, true) == 101);        // a Lite: its 100mW AUTO
    CHECK(ar_pwr_fit(AR_AIR_LITE, 501) == 101);              // as before
    // The P1 Air: up to 200 mW, with kestrel-air's levels; stock's Lite list without them
    CHECK(ar_pwr_fit(AR_AIR_P1, 501, true) == 201);
    CHECK(ar_pwr_fit(AR_AIR_P1, 317, true) == 201);
    CHECK(ar_pwr_fit(AR_AIR_P1, 316, true) == 200);
    CHECK(ar_pwr_fit(AR_AIR_P1, 150, true) == 150);
    CHECK(ar_pwr_fit(AR_AIR_P1, 501) == 101);

    // The level row steps through the mode's levels, lowest first.
    int t[kArPwrTopsMax];
    int n = ar_pwr_tops(AR_AIR_LITE_PLUS, true, false, t);
    CHECK(n == 6 && t[0] == 25 && t[1] == 100 && t[2] == 150 && t[3] == 200 && t[4] == 316 && t[5] == 500);
    n = ar_pwr_tops(AR_AIR_LITE_PLUS, true, true, t);
    CHECK(n == 6 && t[0] == 25 && t[1] == 100 && t[2] == 150 && t[3] == 200 && t[4] == 316 && t[5] == 500);
    // kestrel-air's: the same levels in both modes (the P1 Air up to 200 mW, a Lite 100).
    for (const bool automode : {false, true}) {
        n = ar_pwr_tops(AR_AIR_P1, true, automode, t);
        CHECK(n == 4 && t[0] == 25 && t[1] == 100 && t[2] == 150 && t[3] == 200);
        n = ar_pwr_tops(AR_AIR_LITE, true, automode, t);
        CHECK(n == 2 && t[0] == 25 && t[1] == 100);
    }
    n = ar_pwr_tops(AR_AIR_LITE_PLUS, false, true, t);
    CHECK(n == 1 && t[0] == 500);                            // stock: AUTO at 500 only
    n = ar_pwr_tops(AR_AIR_LITE, false, true, t);
    CHECK(n == 1 && t[0] == 100);

    // 400 mW only where the air unit says it takes it (fine 2).
    n = ar_pwr_tops(AR_AIR_LITE_PLUS, 2, false, t);
    CHECK(n == 7 && t[4] == 316 && t[5] == 400 && t[6] == 500);
    n = ar_pwr_tops(AR_AIR_LITE_PLUS, 2, true, t);
    CHECK(n == 7 && t[4] == 316 && t[5] == 400 && t[6] == 500);   // AUTO at every level too
    CHECK(ar_pwr_fit(AR_AIR_LITE_PLUS, 400, 2) == 400 && ar_pwr_fit(AR_AIR_LITE_PLUS, 401, 2) == 401);
    CHECK(ar_pwr_fit(AR_AIR_LITE_PLUS, 400, 1) == 316 && ar_pwr_fit(AR_AIR_LITE_PLUS, 401, 1) == 317);
    CHECK(ar_pwr_fit(AR_AIR_LITE_PLUS, 400) == 200);          // stock's list
    CHECK(ar_pwr_fit(AR_AIR_LITE, 401, 2) == 101);            // a Lite: still its 100mW AUTO
    CHECK(ar_pwr_pick(AR_AIR_LITE_PLUS, 2, true, 400) == 401);
    CHECK(ar_air_dbm_from_mw(400) == 25 && ar_air_mw_from_dbm(25) == 400);
    CHECK(kArPwrLevels[ar_pwr_index(400)].dbm == 23 && ar_pwr_mw_from_dbm(23) == 400);

    // The mode row: the same level where the mode has it, else the next one up.
    CHECK(ar_pwr_pick(AR_AIR_LITE_PLUS, true, true, 316) == 317);
    CHECK(ar_pwr_pick(AR_AIR_LITE_PLUS, true, false, 316) == 316);
    CHECK(ar_pwr_pick(AR_AIR_LITE_PLUS, true, true, 25) == 26);     // 25 mW AUTO
    CHECK(ar_pwr_pick(AR_AIR_P1, true, true, 25) == 26 && ar_pwr_pick(AR_AIR_P1, true, true, 100) == 101);
    CHECK(ar_pwr_pick(AR_AIR_P1, true, false, 25) == 25);
    CHECK(ar_pwr_pick(AR_AIR_LITE_PLUS, false, true, 200) == 501);  // stock: 500mW AUTO is the only one
    CHECK(ar_pwr_pick(AR_AIR_LITE_PLUS, false, false, 500) == 500);
    CHECK(ar_pwr_pick(AR_AIR_LITE, false, true, 25) == 101);         // stock's Lite list: 100mW AUTO only

    // On the wire: 25 mW AUTO is 25 held, every other level its own value.
    CHECK(ar_pwr_wire(26) == 25 && ar_pwr_wire(25) == 25 && ar_pwr_wire(101) == 101 && ar_pwr_wire(201) == 201);
    for (const auto &l : kArPwrLevels) CHECK(ar_pwr_wire(l.mw) == (l.mw == 26 ? 25 : l.mw));
    CHECK(ar_pwr_fit(AR_AIR_P1, 26, true) == 26 && ar_pwr_fit(AR_AIR_P1, 26) == 25);   // stock's list has no 25 AUTO

    // Round trip of the two rows over one value.
    int v = 316;
    v = ar_pwr_pick(AR_AIR_LITE_PLUS, true, true, ar_pwr_top(v));     // FIXED -> AUTO
    CHECK(v == 317);
    v = ar_pwr_pick(AR_AIR_LITE_PLUS, true, false, ar_pwr_top(v));    // AUTO -> FIXED
    CHECK(v == 316);

    // The air unit's radio level for a level, and back.
    CHECK(ar_air_dbm_from_mw(316) == 24 && ar_air_dbm_from_mw(500) == 26 && ar_air_dbm_from_mw(100) == 19);
    CHECK(ar_air_dbm_from_mw(25) == 13 && ar_air_dbm_from_mw(150) == 21 && ar_air_dbm_from_mw(200) == 22);
    for (const int mw : {25, 100, 150, 200, 316, 500}) CHECK(ar_air_mw_from_dbm(ar_air_dbm_from_mw(mw)) == mw);

    // The VRX's own level for the new ones sits 3 dB under the label, as stock's.
    CHECK(kArPwrLevels[ar_pwr_index(316)].dbm == 22 && kArPwrLevels[ar_pwr_index(150)].dbm == 19);

    std::printf(fails ? "%d FAILED\n" : "power_test ok\n", fails);
    return fails != 0;
}

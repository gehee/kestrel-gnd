// Host test of the elements left out of the Betaflight canvas under the
// canopy (BfTelem::Elem, draw_region's hide), as Betaflight draws them.
//   g++ -std=c++17 -I. -o msp_osd_hide_test tools/test/msp_osd_hide_test.cpp msp_osd.cpp msp_telem.cpp stab/imu_stream.cpp -lpthread && ./msp_osd_hide_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "msp_osd.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Row { int r, c; std::string s; };

// One screen of rows, then what draw_region draws with `hide`, one string per
// row (spans joined, cells not drawn as spaces).
static std::vector<std::string> drawn(const std::vector<Row>& rows, unsigned hide, BfTelem* t = nullptr,
                                      bool analog = false) {
    static MspOsd o;
    o.reset_screen();
    if (analog) {   // MSP_ANALOG from the flight controller: 16.8 V, 12.34 A
        const uint8_t a[] = {168, 0, 0, 0, 2, 0xd2, 0x04, 0x90, 0x06};
        o.handle_msp_frame(110, a, sizeof a);
    }
    const uint8_t clr[] = {2};
    o.handle_msp_frame(182, clr, sizeof clr);
    for (const Row& w : rows) {
        std::vector<uint8_t> p = {3, (uint8_t)w.r, (uint8_t)w.c, 0};
        p.insert(p.end(), w.s.begin(), w.s.end());
        o.handle_msp_frame(182, p.data(), p.size());
    }
    const uint8_t draw[] = {4};
    o.handle_msp_frame(182, draw, sizeof draw);
    if (t) *t = o.get_telem();
    std::vector<std::string> out(MspOsd::grid_h(), std::string(MspOsd::grid_w(), ' '));
    o.set_cell_size(1.0f, 1.0f);
    o.draw_region(0, 0, MspOsd::grid_h(), MspOsd::grid_w(), 0.0f, 0.0f,
                  [&](float x, float y, const std::vector<uint16_t>& span) {
                      int r = (int)-y, c = (int)x;
                      for (size_t i = 0; i < span.size(); i++)
                          out[r][c + i] = span[i] < 128 ? (char)span[i] : '#';
                  }, hide);
    for (auto& s : out) s.erase(s.find_last_not_of(' ') + 1);
    return out;
}

int main() {
    const unsigned canopy = BfTelem::kElCellV | BfTelem::kElAmps | BfTelem::kElMode |
                            BfTelem::kElSpeed | BfTelem::kElLq | BfTelem::kElArm;
    const std::vector<Row> scr = {
        {1, 2, "\x92" "3.83\x06"},          // average cell voltage, battery icon
        {2, 2, "\x90" "16.4\x06"},          // pack voltage
        {3, 2, " 12.34\x9a"},               // current
        {4, 2, "ANGL"},                     // flight mode
        {5, 2, "\x70" " 42\x9e"},           // GPS speed, km/h
        {6, 2, "\x7b" "2:99"},              // Link Quality, CRSF
        {7, 2, "\x01" "99"},                // RSSI
        {8, 2, "DISARMED"},
        {9, 2, "\x9c" "01:23"},             // flight timer
        {10, 2, "12.3\x0c"},                // altitude
        {11, 2, "CRAFT NAME"},
    };
    BfTelem t;
    std::vector<std::string> all = drawn(scr, 0, &t);
    CHECK(t.have_cell_v && t.have_pack_v && t.have_amps && t.have_mode && t.have_lq && t.have_timer);
    CHECK(t.have_speed && t.speed_kmh > 41.9f && t.speed_kmh < 42.1f);    // 0x9E, not a stick sprite
    // All read off the canvas here: no MSP replies in this test.
    CHECK(t.osd_src == (BfTelem::kElCellV | BfTelem::kElPackV | BfTelem::kElAmps | BfTelem::kElMode |
                        BfTelem::kElSpeed | BfTelem::kElLq | BfTelem::kElArm | BfTelem::kElTimer));
    for (int r = 1; r <= 11; r++) CHECK(!all[r].empty());               // canopy off: all drawn

    std::vector<std::string> c = drawn(scr, canopy);
    CHECK(c[1].empty());                     // cell voltage, icon too
    CHECK(!c[2].empty());                    // pack voltage: the canopy shows per cell
    CHECK(c[3].empty());
    CHECK(c[4].empty());
    CHECK(c[5].empty());                     // speed, SYM_SPEED too
    CHECK(c[6].empty());
    CHECK(!c[7].empty());                    // RSSI stays
    CHECK(c[8].empty());
    CHECK(!c[9].empty());                    // timer: only armed
    CHECK(!c[10].empty() && !c[11].empty()); // altitude, craft name stay

    std::vector<std::string> p = drawn(scr, (canopy & ~BfTelem::kElCellV) | BfTelem::kElPackV | BfTelem::kElTimer);
    CHECK(!p[1].empty() && p[2].empty() && p[9].empty());

    // Current from MSP_ANALOG: the canopy's figure is the FC's, so the
    // element stays; the cell voltage is still the canvas's, so it goes.
    BfTelem ta;
    drawn(scr, canopy, &ta, true);
    CHECK(!(ta.osd_src & BfTelem::kElAmps) && (ta.osd_src & BfTelem::kElCellV));
    std::vector<std::string> hm = drawn(scr, canopy & ta.osd_src, nullptr, true);
    CHECK(!hm[3].empty() && hm[1].empty());

    // Two elements on one row: only the hidden one goes.
    std::vector<std::string> two = drawn({{3, 2, "ANGL  " "\x01" "87"}}, canopy);
    CHECK(two[3].find("ANGL") == std::string::npos && two[3].find("87") != std::string::npos);

    std::printf(fails ? "%d failures\n" : "all passed\n", fails);
    return fails != 0;
}

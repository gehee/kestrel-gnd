// Host test of the Link Quality element as Betaflight 4.2-4.5 draws it
// (osd_elements.c osdElementLinkQuality), scraped off the DisplayPort grid.
//   g++ -std=c++17 -I. -o msp_osd_lq_test tools/test/msp_osd_lq_test.cpp msp_osd.cpp msp_telem.cpp motion/imu_stream.cpp -lpthread && ./msp_osd_lq_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "msp/msp_osd.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// One screen: clear, the string at row 5 col 3, draw.
static BfTelem screen(const std::string& s) {
    MspOsd o;
    const uint8_t clr[] = {2};
    o.handle_msp_frame(182, clr, sizeof clr);
    std::vector<uint8_t> w = {3, 5, 3, 0};
    w.insert(w.end(), s.begin(), s.end());
    o.handle_msp_frame(182, w.data(), w.size());
    const uint8_t draw[] = {4};
    o.handle_msp_frame(182, draw, sizeof draw);
    return o.get_telem();
}

int main() {
    const std::string lq = "\x7b";
    BfTelem t;
    t = screen(lq + "2:99");  CHECK(t.have_lq && t.lq_pct == 99);    // CRSF / ELRS, every version
    t = screen(lq + "2:100"); CHECK(t.have_lq && t.lq_pct == 100);
    t = screen(lq + "1: 7");  CHECK(t.have_lq && t.lq_pct == 7);
    t = screen(lq + "9");     CHECK(t.have_lq && t.lq_pct == 95);    // other receivers: tenths
    t = screen(lq + "0");     CHECK(t.have_lq && t.lq_pct == 5);
    t = screen(lq + "4  00:12"); CHECK(t.have_lq && t.lq_pct == 45); // a timer on the same row
    t = screen(lq + "75");    CHECK(t.have_lq && t.lq_pct == 75);    // GHST (4.3+), 0..100
    t = screen(lq + " 7");    CHECK(t.have_lq && t.lq_pct == 7);
    std::printf(fails ? "%d failures\n" : "all passed\n", fails);
    return fails != 0;
}

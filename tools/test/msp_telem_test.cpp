// Host test of msp_telem.cpp: payloads as Betaflight builds them.
//   g++ -std=c++17 -I. -o msp_telem_test tools/test/msp_telem_test.cpp msp_telem.cpp && ./msp_telem_test
#include <cstdio>
#include <cstring>
#include <vector>
#include "msp_telem.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void put16(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void put32(std::vector<uint8_t>& v, int32_t x) { put16(v, x & 0xffff); put16(v, (x >> 16) & 0xffff); }
static unsigned feed(MspTelem& t, int fn, const std::vector<uint8_t>& p) { return t.handle((uint16_t)fn, p.data(), p.size()); }

int main() {
    MspTelem t;
    // BATTERY_STATE: 4 cells, 1300 mAh pack, 16.2 V, 800 mAh drawn, 12.34 A
    std::vector<uint8_t> b; b.push_back(4); put16(b, 1300); b.push_back(162); put16(b, 800); put16(b, 1234); b.push_back(0); put16(b, 1623);
    CHECK(feed(t, 130, b) == MspTelem::G_BATT);
    CHECK(t.state().cells == 4 && t.state().capacity_mah == 1300 && t.state().mah_drawn == 800);
    CHECK(t.state().amps > 12.33f && t.state().amps < 12.35f && t.state().pack_v > 16.22f && t.state().pack_v < 16.24f);
    std::vector<uint8_t> shortb(b.begin(), b.begin() + 10);
    CHECK(feed(t, 130, shortb) == 0);                       // too short: ignored
    b[9] = b[10] = 0;                                       // no 0.01 V field: the legacy 0.1 V one
    feed(t, 130, b);
    CHECK(t.state().pack_v > 16.19f && t.state().pack_v < 16.21f);

    // ALTITUDE: 123.45 m, climbing 1.5 m/s
    std::vector<uint8_t> al; put32(al, 12345); put16(al, 150);
    CHECK(feed(t, 109, al) == MspTelem::G_ALT && t.state().alt_m > 123.4f && t.state().alt_m < 123.5f && t.state().vario_ms == 1.5f);

    // RAW_GPS: 3D fix, 11 sats, 45.5, -73.6, 36 m, 10 m/s
    std::vector<uint8_t> g; g.push_back(2); g.push_back(11); put32(g, 455000000); put32(g, -736000000); put16(g, 36); put16(g, 1000); put16(g, 900);
    CHECK(feed(t, 106, g) == MspTelem::G_GPS);
    CHECK(t.state().fix == 2 && t.state().sats == 11 && t.state().lat > 45.49 && t.state().lon < -73.59);
    CHECK(t.state().gps_speed_kmh > 35.9f && t.state().gps_speed_kmh < 36.1f);

    // COMP_GPS: 250 m, bearing home -45 -> 315
    std::vector<uint8_t> h; put16(h, 250); put16(h, -45); h.push_back(1);
    CHECK(feed(t, 107, h) == MspTelem::G_HOME && t.state().home_dist_m == 250 && t.state().home_dir_deg == 315);

    // Modes: boxes (ids) ARM 0, ANGLE 1, HORIZON 2, AIR MODE 28, FAILSAFE 27, GPS RESCUE 46 - bit i is box i.
    std::vector<uint8_t> boxes = { 0, 1, 2, 28, 27, 46 };
    CHECK(feed(t, 119, boxes) == MspTelem::G_MODES);
    CHECK(!t.state().have_modes);                           // no flags yet
    auto status = [&](uint32_t flags) { std::vector<uint8_t> s(22, 0); memcpy(&s[6], &flags, 4); return s; };
    feed(t, 101, status(0));
    CHECK(t.state().have_arm && !t.state().armed && !strcmp(t.state().mode, "ACRO"));
    feed(t, 101, status(1 | 1 << 3));                       // armed, air mode: still acro
    CHECK(t.state().armed && !strcmp(t.state().mode, "ACRO"));
    feed(t, 150, status(1 | 1 << 1));                       // armed + angle
    CHECK(!strcmp(t.state().mode, "ANGL"));
    feed(t, 101, status(1 | 1 << 2));
    CHECK(!strcmp(t.state().mode, "HOR"));
    feed(t, 101, status(1 | 1 << 1 | 1 << 4));              // failsafe wins over angle
    CHECK(!strcmp(t.state().mode, "FAIL"));
    feed(t, 101, status(1 | 1 << 1 | 1 << 5));
    CHECK(!strcmp(t.state().mode, "RESC"));
    std::vector<uint8_t> shorts(9, 0);
    CHECK(feed(t, 101, shorts) == 0);

    std::vector<uint8_t> a; put16(a, -125); put16(a, 73); put16(a, -90);
    CHECK(t.handle(108, a.data(), a.size()) == 0);          // ATTITUDE is not polled any more: the camera's IMU has it
    CHECK(t.handle(110, a.data(), a.size()) == 0);          // ANALOG is not this module's
    t.reset();
    CHECK(!t.state().have_batt && !t.state().have_modes);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}

// Unit test of radio/model.cpp, the parts the radio carries (system/), with a
// fake radio (radio::Link) in place of the AR8030: what the radio reports
// reaches each part's reader on its terms - each part's state as its newest
// value (defaults before the first), its watcher woken when it changes and
// not otherwise, the air unit's reports counted, the video rate and the scans
// every one in order and waking, the resends every one without a wake, the FC
// serial bridge's bytes with the screen's restart in order between them, the
// air unit's IMU samples with the camera angle in order - the distance is
// zeroed at the first arming after 3 s of standby, the changes asked of the
// parts reach the radio numbered and normalized, in order, their tickets
// carrying the value as it will be held, and the settings among them are
// saved as they are asked; the radio's threads and a reader can run at once
// (run it under ThreadSanitizer too). Nothing is written: Settings has no
// file here.
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "base/settings.hpp"
#include "radio/model.hpp"
#include "radio/settings.hpp"
#include "radio/streams.hpp"

// A model, as main() makes it.
struct Rig {
    Settings settings;                  // no file: nothing is written
    radio::Model model{settings};
};

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// A radio that is not one: it holds what the commands set, and counts.
struct FakeLink : radio::Link {
    std::vector<radio::Command> applied;
    radio::State h;
    bool scan = false;
    void run() override {}
    void apply(const radio::Command& c) override {
        applied.push_back(c);
        if (const auto* m = std::get_if<radio::cmd::MaxBitrate>(&c)) h.max_kbps = m->kbps;
        if (const auto* m = std::get_if<radio::cmd::MaxBandwidth>(&c)) h.max_bw_mhz = m->mhz;
        if (const auto* m = std::get_if<radio::cmd::Firmware>(&c)) h.radio_fw = m->firmware;
        if (const auto* m = std::get_if<radio::cmd::ChannelHop>(&c)) h.chan_hop = m->hop;
        if (const auto* m = std::get_if<radio::cmd::ScanScreen>(&c)) scan = m->open;
    }
    radio::State held() const override { return h; }
};

static gnd::LinkStats linked(const char* peer) {
    gnd::LinkStats s{};
    s.state = gnd::kLinkUp;
    snprintf(s.peer_mac, sizeof(s.peer_mac), "%s", peer);
    return s;
}

// The HUD's terms (hud/osd.cpp): the parts it watches, on one wake.
struct HudLike {
    bus::Waker w;
    radio::Model& m;
    uint32_t radio_seen = 0, air_seen = 0;
    explicit HudLike(Rig& b) : m(b.model) {
        m.radio().watch(&w);
        m.air_radio().watch(&w);
        m.encoder().watch(&w);
        m.air_unit().watch(&w);
        m.camera().watch(&w);
        m.fc().watch(&w);
    }
    ~HudLike() {
        m.radio().watch(nullptr);
        m.air_radio().watch(nullptr);
        m.encoder().watch(nullptr);
        m.air_unit().watch(nullptr);
        m.camera().watch(nullptr);
        m.fc().watch(nullptr);
    }
    // The link's parts or the air unit's moved since the last look.
    bool take_changed() {
        const uint32_t rv = m.radio().version() + m.air_radio().version() + m.encoder().version();
        const uint32_t av = m.air_unit().version() + m.camera().version();
        const bool moved = rv != radio_seen || av != air_seen;
        radio_seen = rv;
        air_seen = av;
        return moved;
    }
};

static gnd::RadioState radio_of(Rig& b) { return b.model.radio().state(); }
static air::UnitState unit_of(Rig& b) { return b.model.air_unit().state(); }

static void defaults_test(Rig& b) {
    const gnd::RadioState rs = radio_of(b);
    CHECK(b.model.radio().version() == 0 && rs.dist_raw == -1 && rs.dist_m == -1 && rs.video_mbps == 0.0f &&
          rs.link.bw_step == 0 && rs.max_mcs == -1);
    CHECK(b.model.air_unit().version() == 0 && unit_of(b).features == 0 && !unit_of(b).vtx_temp_valid);
    const air::CameraState c = b.model.camera().state();
    CHECK(c.video_mode == -1 && c.mode_n == 0);
}

static void state_test(Rig& b, HudLike& h) {
    while (h.w.wait(1000)) {}
    h.take_changed();
    // The air unit's reports: each counted, the newest kept, a wake.
    b.model.update_camera([](air::CameraState& c) { c.video_mode = 3; c.mode_n++; });
    b.model.update_camera([](air::CameraState& c) { c.video_mode = 4; c.mode_n++; });
    b.model.update_unit([](air::UnitState& u) { u.standby = true; u.standby_n++; });
    CHECK(h.w.wait(1000) && h.take_changed());
    const air::CameraState c = b.model.camera().state();
    CHECK(c.video_mode == 4 && c.mode_n == 2 && unit_of(b).standby && unit_of(b).standby_n == 1);
    // What the radio holds: published to the parts it belongs to when it
    // changes, not otherwise.
    radio::State st;
    st.max_kbps = 5;
    st.tx_power_want_mw = 200;
    st.chan_hop = 1;
    b.model.held(st);
    CHECK(h.take_changed() && b.model.encoder().state().max_kbps == 5 &&
          b.model.air_radio().state().tx_power_want_mw == 200 && radio_of(b).chan_hop == 1);
    b.model.held(st);
    CHECK(!h.take_changed());
    // The link's state, as the poll repeats it: published when it changes.
    b.model.link_state(gnd::kLinkUp);
    CHECK(h.take_changed());
    b.model.link_state(gnd::kLinkUp);
    CHECK(!h.take_changed());
    b.model.link_state(0);
    while (h.w.wait(1000)) {}
    gnd::ScanSample sc{};
    sc.chan_num = 3;
    sc.usable[1] = 1;
    b.model.scan(sc);
    CHECK(h.w.wait(1000));                      // each scan wakes it
    gnd::ScanSample got{};
    CHECK(b.model.radio().take_scan(got) && got.chan_num == 3 && got.usable[1] && !got.usable[0]);
    // The video rate: every second's, in order, and the radio's figure.
    b.model.video_bandwidth(12.5f);
    b.model.video_bandwidth(13.0f);
    float m = 0;
    CHECK(b.model.radio().take_rate(m) && m == 12.5f && b.model.radio().take_rate(m) && m == 13.0f &&
          !b.model.radio().take_rate(m));
    CHECK(radio_of(b).video_mbps == 13.0f);
    // The resends: every one, without a wake.
    while (h.w.wait(1000)) {}
    b.model.resend({100, 1});
    b.model.resend({200, 3});
    CHECK(!h.w.wait(1000));
    air::Resend rr{};
    air::Radio& ar = b.model.air_radio();
    CHECK(ar.take_resend(rr) && rr.t_us == 100 && ar.take_resend(rr) && rr.v == 3 && !ar.take_resend(rr));
}

static void drain(Rig& b) {
    air::FcChunk c;
    while (b.model.fc().take(c)) {}
}

// The radio's FC serial bridge (radio/streams.hpp), as the HUD takes it.
static void fc_test(Rig& b, HudLike& h) {
    radio::FcFeed& w = b.model.fc();
    air::Fc& in = w;
    drain(b);
    while (h.w.wait(1000)) {}
    const uint8_t a[4] = {1, 2, 3, 4}, two[2] = {5, 6};
    w.bytes(a, 4);
    CHECK(h.w.wait(1000));                      // into an empty queue: woken
    w.new_session();                            // a link-up: the FC's screen starts over here
    w.bytes(two, 2);
    CHECK(!h.w.wait(1000) && in.pending());     // not empty: no wake for more
    air::FcChunk c;
    CHECK(in.take(c) && !c.screen_reset && c.n == 4 && c.b[3] == 4);
    CHECK(in.take(c) && c.screen_reset && c.n == 0);
    CHECK(in.take(c) && !c.screen_reset && c.n == 2 && c.b[1] == 6);
    CHECK(!in.take(c) && !in.pending());
    // A long write comes in chunks, in order.
    uint8_t big[300];
    for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;
    w.bytes(big, sizeof(big));
    std::vector<uint8_t> all;
    int chunks = 0;
    while (in.take(c)) { all.insert(all.end(), c.b, c.b + c.n); chunks++; }
    CHECK(chunks == 3 && all.size() == 300 && all[0] == 0 && all[299] == (uint8_t)299);
}

// A screen restart that finds the queue full is not lost: it goes in first
// when there is room, before newer bytes (the ones in between go).
static void fc_full_test() {
    radio::FcFeed w;
    bus::Waker wake;
    w.watch(&wake);
    const uint8_t x[1] = {7}, y[1] = {9};
    for (int i = 0; i < 2048; i++) w.bytes(x, 1);   // full
    w.new_session();                                // a new session: a restart, no room for it
    CHECK(w.dropped() == 1);
    w.bytes(x, 1);                                  // still full: dropped, the restart still owed
    air::FcChunk c;
    CHECK(w.take(c) && w.take(c) && !c.screen_reset);
    w.bytes(y, 1);                                  // room: the restart, then these
    for (int i = 0; i < 2046; i++) CHECK(w.take(c) && !c.screen_reset);
    CHECK(w.take(c) && c.screen_reset);
    CHECK(w.take(c) && !c.screen_reset && c.n == 1 && c.b[0] == 9);
    CHECK(!w.take(c));
    w.bytes(y, 1);                                  // nothing owed now: just the bytes
    CHECK(w.take(c) && !c.screen_reset && !w.take(c));
}

static void distance_test(Rig& b) {
    b.model.link_stats(linked("AA:BB:CC:01"));
    drain(b);
    b.model.distance(100);
    CHECK(radio_of(b).dist_raw == 100 && radio_of(b).dist_m == -2);    // not zeroed yet
    b.model.low_power(true);                          // standby
    b.model.distance(110);                            // kept for the zero
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    b.model.low_power(false);                         // armed after 3 s of standby: zeroed
    b.model.distance(150);
    CHECK(radio_of(b).dist_m == 40);
    CHECK(!unit_of(b).low_power);
    b.model.low_power(true);
    b.model.low_power(false);                         // a re-arm keeps the zero
    CHECK(radio_of(b).dist_m == 40);
    // Another aircraft: its distance unzeroed.
    b.model.link_stats(linked("AA:BB:CC:02"));
    CHECK(radio_of(b).dist_m == -2);
}

// An SEI with one kestrel-air IMU message: n samples 1 ms apart from t0.
static std::vector<uint8_t> imu_sei(uint64_t t0, int n) {
    std::vector<uint8_t> pl;
    auto p16 = [&](unsigned x) { pl.push_back((uint8_t)x); pl.push_back((uint8_t)(x >> 8)); };
    auto p64 = [&](uint64_t x) { for (int i = 0; i < 8; i++) pl.push_back((uint8_t)(x >> (8 * i))); };
    const char uuid[] = "kestrel-air-IMU1";
    pl.insert(pl.end(), uuid, uuid + 16);
    pl.push_back(1); pl.push_back(0); p16(7);
    p64(t0); p64(t0);
    p16(n); p16(164); p16(2048);
    for (int k = 0; k < n; k++) { p16(k * 1000); p16(1); p16(2); p16(3); p16(4); p16(5); p16(6); }
    std::vector<uint8_t> raw = { 5 };
    for (size_t left = pl.size(); ; left -= 255) {
        if (left < 255) { raw.push_back((uint8_t)left); break; }
        raw.push_back(0xff);
    }
    raw.insert(raw.end(), pl.begin(), pl.end());
    raw.push_back(0x80);
    std::vector<uint8_t> nal = { 39 << 1, 1 };
    int zeros = 0;
    for (uint8_t c : raw) {                         // emulation prevention
        if (zeros >= 2 && c <= 3) { nal.push_back(3); zeros = 0; }
        nal.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return nal;
}

// The radio's IMU stream (radio/streams.hpp), as the IMU reader takes it.
static void imu_test(Rig& b, bus::Waker& ir) {
    radio::ImuFeed& w = b.model.imu();
    air::Imu& imu = w;
    air::ImuBatch x;
    while (imu.take(x)) {}
    while (ir.wait(1000)) {}
    const auto v = imu_sei(5000000, 3);
    w.sei(v.data(), v.size());
    CHECK(ir.wait(1000));
    CHECK(imu.take(x) && x.message && x.n == 3 && x.t0_us == 5000000 && x.camera_angle == 0);
    CHECK(x.s[2].dt_us == 2000 && x.s[2].gyro[1] == 2 && x.s[2].accel[2] == 6 && x.gyro_lsb == 16.4 && x.accel_lsb == 2048);
    CHECK(x.rx_ms > 0 && !imu.take(x));
    // The angle, then a message: in that order, the message carrying it too.
    w.camera_angle(180);
    w.sei(v.data(), v.size());
    CHECK(imu.take(x) && !x.message && x.n == 0 && x.camera_angle == 180);
    CHECK(imu.take(x) && x.message && x.camera_angle == 180);
    w.camera_angle(0);
    CHECK(imu.take(x) && x.camera_angle == 0 && !imu.take(x));
    // Another SEI: nothing.
    std::vector<uint8_t> other = v;
    const char* uuid = "kestrel-air-IMU1";
    auto u = std::search(other.begin(), other.end(), uuid, uuid + 16);
    CHECK(u != other.end());
    *u = 'X';
    w.sei(other.data(), other.size());
    CHECK(!imu.take(x));
}

static void commands_test(Rig& b, HudLike& h) {
    FakeLink f;
    f.h.max_kbps = 1000;
    b.model.take_commands(f);                         // none yet: what it holds goes out
    CHECK(b.model.encoder().state().max_kbps == 1000 && b.model.encoder().state().taken == 0);
    // Asked: numbered in order, normalized, the ticket's value as it will be
    // held; not carried out until the radio comes round (the menu shows what
    // it asked until the part's taken passes it).
    const Ticket t1 = b.model.encoder().set_max_bitrate(5000);
    const Ticket t2 = b.model.radio().set_max_mcs(40);      // out of range: none
    const Ticket t3 = b.model.radio().set_channel(5805);    // FIXED: AUTO HOP off with it
    CHECK(t1.seq > 0 && t2.seq == t1.seq + 1 && t3.seq == t2.seq + 1);
    CHECK(t1.value == 5000 && t2.value == -1 && t3.value == 0);
    CHECK(f.applied.empty() && b.model.encoder().state().taken < t1.seq);
    h.take_changed();
    b.model.take_commands(f);                         // the radio's thread, between its steps
    CHECK(f.applied.size() == 3 && std::get<radio::cmd::MaxBitrate>(f.applied[0]).kbps == 5000);
    CHECK(std::get<radio::cmd::MaxMcs>(f.applied[1]).mcs == -1 && std::get<radio::cmd::RfControl>(f.applied[2]).fixed_mhz == 5805);
    CHECK(b.model.encoder().state().taken == t3.seq && b.model.radio().state().taken == t3.seq &&
          b.model.encoder().state().max_kbps == 5000);
    CHECK(h.take_changed());                    // what took effect: the HUD woken
    // Max Bandwidth: 0 (no cap), 5, 10, 20 or 40; the rest on or off.
    CHECK(b.model.radio().set_max_bandwidth(30).value == 40 && b.model.radio().set_max_bandwidth(0).value == 0);
    CHECK(b.model.radio().set_frame(1).value == 2 && b.model.radio().set_firmware(3).value == 0);
    CHECK(b.model.radio().set_channel(0).seq == 0);   // no channel: AUTO HOP not touched
    b.model.take_commands(f);
    // The scan screen's: carried out like the others.
    b.model.radio().scan_screen(true);
    b.model.take_commands(f);
    CHECK(std::holds_alternative<radio::cmd::ScanScreen>(f.applied.back()) && f.scan);
    // The settings among them saved as they are asked, before the radio takes them.
    Settings& st = b.settings;
    namespace rs = radio::setting;
    b.model.radio().set_max_mcs(7);
    b.model.radio().set_channel_auto(true);
    CHECK(st.get(rs::ar8030_max_mcs) == 7 && st.text(rs::ar8030_chan_mhz) == "0" && st.get(rs::ar8030_chan_hop) == 1);
    b.model.radio().set_channel(5865);
    CHECK(st.get(rs::ar8030_chan_mhz) == 5865 && st.text(rs::ar8030_chan_hop) == "0");
    b.model.encoder().set_max_bitrate(4321);
    b.model.radio().set_firmware(1);
    CHECK(st.get(rs::ar8030_max_kbps) == 4321 && st.text(rs::ar8030_firmware) == "1");
    b.model.take_commands(f);
    // The air unit's info: published by the radio, read without its lock,
    // its capabilities named.
    air::Info ai;
    ai.kestrel = true;
    ai.kv_n = 1;
    snprintf(ai.kv[0].key, sizeof(ai.kv[0].key), "model");
    snprintf(ai.kv[0].value, sizeof(ai.kv[0].value), "Ascent Lite+");
    ai.caps_n = 1;
    snprintf(ai.caps[0], sizeof(ai.caps[0]), "Radio Clock");
    b.model.air_info(ai);
    const air::UnitState au = unit_of(b);
    CHECK(au.info.kestrel && au.info.get("model") == "Ascent Lite+" && au.info.get("os").empty());
    CHECK(au.info.caps_n == 1 && std::string(au.info.caps[0]) == "Radio Clock");
}

// The radio's three threads and a reader at once.
static void threads_test(Rig& b, HudLike& h) {
    std::atomic<bool> stop{false};
    radio::FcFeed& fw = b.model.fc();
    std::thread stats([&] {
        for (int i = 0; !stop; i++) {
            b.model.link_stats(linked(i % 2 ? "AA:BB:CC:03" : "AA:BB:CC:04"));
            b.model.distance(100 + i % 50);
            b.model.low_power(i % 3 == 0);
            b.model.scan(gnd::ScanSample{});
        }
    });
    std::thread events([&] {
        for (int i = 0; !stop; i++) {
            b.model.link_state(i % 2 ? gnd::kLinkUp : 0);
            b.model.rx_mcs(i % 12);
            b.model.update_camera([&](air::CameraState& c) { c.dnr3d = i; c.dnr_n++; });
            b.model.video_bandwidth((float)i);
            fw.bytes((const uint8_t*)"ab", 2);       // a second writer of the FC's bytes
        }
    });
    std::thread radio_thread([&] {
        const uint8_t x[16] = {};
        for (int i = 0; !stop; i++) {
            b.model.air_delay((float)i);
            fw.bytes(x, sizeof(x));
            b.model.update_unit([&](air::UnitState& a) { a.vtx_cpu_c = (float)i; a.vtx_temp_valid = true; });
            FakeLink f;
            b.model.take_commands(f);
        }
    });
    std::thread asker([&] {                       // the menu, the web view, a button
        for (int i = 0; !stop; i++) b.model.radio().set_max_mcs(i % 12);
    });
    uint64_t reads = 0, popped = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
        const gnd::RadioState rs = b.model.radio().state();
        CHECK(rs.dist_m >= -2);
        (void)b.model.air_unit().state();
        (void)b.model.timing().state();
        float m;
        while (b.model.radio().take_rate(m)) popped++;
        gnd::ScanSample sc;
        while (b.model.radio().take_scan(sc)) {}
        drain(b);
        h.take_changed();
        reads++;
    }
    stop = true;
    stats.join();
    events.join();
    radio_thread.join();
    asker.join();
    CHECK(reads > 0 && popped > 0);
}

int main() {
    Rig b;
    defaults_test(b);
    // The IMU's reader alone is not a reader of the radio's state.
    bus::Waker ir;
    b.model.imu().watch(&ir);
    CHECK(b.model.imu().watched() && !b.model.viewers() && !b.model.fc().watched());
    {
        HudLike h(b);
        CHECK(b.model.viewers() && b.model.fc().watched());
        state_test(b, h);
        fc_test(b, h);
        fc_full_test();
        distance_test(b);
        imu_test(b, ir);
        commands_test(b, h);
        threads_test(b, h);
    }
    // The HUD gone: it stopped watching; nothing reaches it.
    CHECK(!b.model.viewers() && !b.model.fc().watched());
    b.model.video_bandwidth(1.0f);
    b.model.imu().watch(nullptr);
    CHECK(!b.model.imu().watched());
    std::printf(fails ? "model_test: %d FAILED\n" : "model_test: all passed\n", fails);
    return fails ? 1 : 0;
}

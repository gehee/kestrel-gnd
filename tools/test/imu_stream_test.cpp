// Host test of motion/imu_stream.cpp: synthetic SEI NALs as kestrel-air --imu builds them,
// read out by the radio (radio/air_imu.cpp) and fed in as the IMU reader does.
//   g++ -std=c++17 -I. -o imu_stream_test tools/test/imu_stream_test.cpp motion/imu_stream.cpp radio/air_imu.cpp && ./imu_stream_test
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "motion/imu_reader.hpp"
#include "motion/imu_stream.hpp"
#include "radio/air_imu.hpp"
#include "radio/streams.hpp"
#include "motion/cam_tilt.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void p16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void p64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; i++) v.push_back((uint8_t)(x >> (8 * i))); }

// n samples 1 ms apart from t0 (us), all with the same gyro reading in LSB (16.4 LSB/dps).
static std::vector<uint8_t> sei(uint64_t t0, int n, int gx, int gy, int gz, int ax = 0, int ay = -2048, int az = 0) {
    std::vector<uint8_t> pl;
    const char uuid[] = "kestrel-air-IMU1";
    pl.insert(pl.end(), uuid, uuid + 16);
    pl.push_back(1); pl.push_back(0); p16(pl, 7);        // version, flags, picture
    p64(pl, t0); p64(pl, t0);                            // capture PTS, first sample time
    p16(pl, n); p16(pl, 164); p16(pl, 2048);             // n, gyro LSB per 0.1 dps, accel LSB per g
    for (int k = 0; k < n; k++) {
        p16(pl, k * 1000);
        p16(pl, (unsigned)(gx & 0xffff)); p16(pl, (unsigned)(gy & 0xffff)); p16(pl, (unsigned)(gz & 0xffff));
        p16(pl, (unsigned)(ax & 0xffff)); p16(pl, (unsigned)(ay & 0xffff)); p16(pl, (unsigned)(az & 0xffff));
    }
    std::vector<uint8_t> raw = { 5 };                               // payload type 5, then the size: 255s and the rest
    size_t left = pl.size();
    while (left >= 255) { raw.push_back(0xff); left -= 255; }
    raw.push_back((uint8_t)left);
    raw.insert(raw.end(), pl.begin(), pl.end());
    raw.push_back(0x80);
    std::vector<uint8_t> nal = { 39 << 1, 1 };                      // SEI NAL header
    int zeros = 0;
    for (uint8_t c : raw) {                                          // emulation prevention
        if (zeros >= 2 && c <= 3) { nal.push_back(3); zeros = 0; }
        nal.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return nal;
}

// The camera angle the radio stamps on each batch.
static int angle = 0;

// One SEI as the radio publishes it (arrival now, the angle) and the IMU reader adds it.
// True if it was the air unit's IMU.
static bool feed(const std::vector<uint8_t>& v) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    return radio::read_imu_sei(v.data(), v.size(), [&](air::ImuBatch& b) {
        b.rx_ms = now_ms;
        b.camera_angle = angle;
        motion::ImuStream::get().add(b);
    });
}

int main() {
    auto& imu = motion::ImuStream::get();
    double w[3];
    CHECK(!imu.recent_rate_dps(20000, w));                           // nothing yet
    // 164 dps in 16.4 LSB/dps units = 2690 LSB: 164 deg/s about the IMU's y axis (the camera's pan).
    for (int m = 0; m < 3; m++) CHECK(feed(sei(1000000 + m * 20000, 20, 0, 2690, 0)));
    CHECK(imu.recent_rate_dps(20000, w));
    CHECK(std::fabs(w[1] - 164.0) < 0.5 && std::fabs(w[0]) < 0.5 && std::fabs(w[2]) < 0.5);   // y is y: right turn positive
    // A turn the other way, a window later: the 20 ms average is all of it.
    feed(sei(1100000, 20, 0, -1640, 0));
    CHECK(imu.recent_rate_dps(15000, w) && std::fabs(w[1] + 100.0) < 0.5);
    // The camera turned over: pan flips.
    angle = 180;
    feed(sei(2000000, 20, 0, 1640, 0));
    CHECK(imu.recent_rate_dps(15000, w) && std::fabs(w[1] + 100.0) < 0.5);
    // Resting gravity gives the camera's tilt: still for 0.6 s with the camera tilted up 25 degrees
    // (2048 LSB = 1 g; the IMU's y reads -1 g when level).
    double down[3];
    auto still = [&](uint64_t t0, int ax, int ay, int az, int messages) {
        for (int m = 0; m < messages; m++) feed(sei(t0 + m * 20000, 20, 0, 0, 0, ax, ay, az));
    };
    imu.reset(); angle = 0;
    still(9000000, 0, (int)(-2048 * std::cos(25 * M_PI / 180)), (int)(-2048 * std::sin(25 * M_PI / 180)), 10);
    CHECK(!imu.resting_down(down));                                     // 0.2 s is not long enough
    still(9200000, 0, (int)(-2048 * std::cos(25 * M_PI / 180)), (int)(-2048 * std::sin(25 * M_PI / 180)), 30);
    CHECK(imu.resting_down(down) && std::fabs(std::asin(-down[2]) * 180 / M_PI - 25.0) < 0.5 && std::fabs(down[0]) < 0.02);
    // Moving (a turn) ends it; a 2 g reading is not gravity.
    feed(sei(9800000, 20, 0, 3000, 0, 0, -2048, 0));
    CHECK(!imu.resting_down(down));
    imu.reset();
    still(10000000, 0, -4096, 0, 40);
    CHECK(!imu.resting_down(down));

    // A message longer than a batch comes in two, counted once, every sample in.
    {
        imu.reset();
        const motion::ImuStream::Status before = imu.status();
        int batches = 0;
        const std::vector<uint8_t> v = sei(20000000, air::ImuBatch::kMax + 1, 0, 1640, 0);
        CHECK(radio::read_imu_sei(v.data(), v.size(), [&](air::ImuBatch& b) {
            batches++;
            b.rx_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
            imu.add(b);
        }));
        const motion::ImuStream::Status after = imu.status();
        CHECK(batches == 2 && after.messages == before.messages + 1 && after.samples == before.samples + air::ImuBatch::kMax + 1);
        CHECK(after.newest_us == 20000000 + air::ImuBatch::kMax * 1000);
        // Not the air unit's: nothing.
        std::vector<uint8_t> other = v;
        const char* uuid = "kestrel-air-IMU1";
        auto u = std::search(other.begin(), other.end(), uuid, uuid + 16);
        CHECK(u != other.end());
        *u = 'X';
        CHECK(!radio::read_imu_sei(other.data(), other.size(), [&](air::ImuBatch&) { batches++; }) && batches == 2);
        // The camera angle alone (the radio's angle change): the orientation starts over.
        air::ImuBatch turn;
        turn.camera_angle = 180;
        imu.add(turn);
        CHECK(imu.status().newest_us == 0 && imu.status().messages == after.messages);
    }

    // A yaw seen by a camera tilted up 25 degrees is pan and roll: put back, it is yaw alone.
    {
        const double a = 25 * M_PI / 180, yaw = 90, roll = 60, pitch = 40;
        double wc[3] = { pitch, yaw * std::cos(a) + roll * std::sin(a), -yaw * std::sin(a) + roll * std::cos(a) };   // camera axes
        double wa[3];
        motion::to_aircraft_rates(wc, 25.0, wa);
        CHECK(std::fabs(wa[0] - pitch) < 1e-6 && std::fabs(wa[1] - yaw) < 1e-6 && std::fabs(wa[2] - roll) < 1e-6);
        motion::to_aircraft_rates(wc, 0.0, wa);                              // no tilt: the camera's own axes
        CHECK(std::fabs(wa[1] - wc[1]) < 1e-9 && std::fabs(wa[2] - wc[2]) < 1e-9);
    }

    // The tilt is taken when the aircraft is armed, from what was read while it rested.
    {
        CamTilt t;
        t.set(10.0);                                                       // saved from before
        t.rest(24.0, 1000000); t.rest(25.0, 2000000); t.rest(25.0, 3000000);
        CHECK(t.deg() == 10.0);                                            // resting alone changes nothing
        CHECK(t.armed(3500000) && std::fabs(t.deg() - 24.2) < 0.6);         // armed: the smoothed reading, saved
        t.rest(60.0, 9000000);                                              // carried about, held tilted: still disarmed
        CHECK(t.deg() < 25.0);
        CHECK(!t.armed(20000000) && t.deg() < 25.0);                        // armed long after the last reading: not trusted
        t.rest(25.0, 30000000);
        CHECK(!t.armed(30500000));                                          // within a degree: nothing to save
    }

    // Through the radio, as the app runs it: published on this thread (the radio's), taken
    // on the IMU reader's, every message in.
    {
        imu.reset();
        const motion::ImuStream::Status before = imu.status();
        {
            radio::ImuFeed feed;                // the radio's, the model's to own
            motion::ImuReader reader(feed);
            feed.camera_angle(0);
            for (int m = 0; m < 50; m++) {
                const std::vector<uint8_t> v = sei(30000000 + m * 20000, 20, 0, 1640, 0);
                feed.sei(v.data(), v.size());
            }
            for (int i = 0; i < 400 && imu.status().messages < before.messages + 50; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const motion::ImuStream::Status after = imu.status();
        CHECK(after.messages == before.messages + 50 && after.samples == before.samples + 1000);
        CHECK(after.newest_us == 30000000 + 49 * 20000 + 19 * 1000);
        CHECK(imu.recent_rate_dps(15000, w) && std::fabs(w[1] - 100.0) < 0.5);
    }

    // No data for 150 ms of this machine's time: not live.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(!imu.recent_rate_dps(20000, w));
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}

// Host test of stab/imu_stream.cpp's recent_rate_dps: a synthetic SEI NAL as kestrel-air --imu builds it.
//   g++ -std=c++17 -I. -Istab -o imu_stream_test tools/test/imu_stream_test.cpp stab/imu_stream.cpp && ./imu_stream_test
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "stab/imu_stream.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void p16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void p64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; i++) v.push_back((uint8_t)(x >> (8 * i))); }

// n samples 1 ms apart from t0 (us), all with the same gyro reading in LSB (16.4 LSB/dps).
static std::vector<uint8_t> sei(uint64_t t0, int n, int gx, int gy, int gz) {
    std::vector<uint8_t> pl;
    const char uuid[] = "kestrel-air-IMU1";
    pl.insert(pl.end(), uuid, uuid + 16);
    pl.push_back(1); pl.push_back(0); p16(pl, 7);        // version, flags, picture
    p64(pl, t0); p64(pl, t0);                            // capture PTS, first sample time
    p16(pl, n); p16(pl, 164); p16(pl, 2048);             // n, gyro LSB per 0.1 dps, accel LSB per g
    for (int k = 0; k < n; k++) {
        p16(pl, k * 1000);
        p16(pl, (unsigned)(gx & 0xffff)); p16(pl, (unsigned)(gy & 0xffff)); p16(pl, (unsigned)(gz & 0xffff));
        p16(pl, 0); p16(pl, 0); p16(pl, 2048);
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

int main() {
    auto& imu = stab::ImuStream::get();
    double w[3];
    CHECK(!imu.recent_rate_dps(20000, w));                           // nothing yet
    // 164 dps in 16.4 LSB/dps units = 2690 LSB: 164 deg/s about the IMU's y axis (the camera's pan).
    for (int m = 0; m < 3; m++) CHECK(imu.on_sei_nal(sei(1000000 + m * 20000, 20, 0, 2690, 0).data(), sei(1000000, 20, 0, 2690, 0).size()));
    CHECK(imu.recent_rate_dps(20000, w));
    CHECK(std::fabs(w[1] - 164.0) < 0.5 && std::fabs(w[0]) < 0.5 && std::fabs(w[2]) < 0.5);   // y is y: right turn positive
    // A turn the other way, a window later: the 20 ms average is all of it.
    imu.on_sei_nal(sei(1100000, 20, 0, -1640, 0).data(), sei(1100000, 20, 0, -1640, 0).size());
    CHECK(imu.recent_rate_dps(15000, w) && std::fabs(w[1] + 100.0) < 0.5);
    // The camera turned over: pan flips.
    imu.set_camera_angle(180);
    imu.on_sei_nal(sei(2000000, 20, 0, 1640, 0).data(), sei(2000000, 20, 0, 1640, 0).size());
    CHECK(imu.recent_rate_dps(15000, w) && std::fabs(w[1] + 100.0) < 0.5);
    // No data for 150 ms of this machine's time: not live.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(!imu.recent_rate_dps(20000, w));
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}

#include "radio/air_imu.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace radio {

namespace {

constexpr char kUuid[17] = "kestrel-air-IMU1";

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

}  // namespace

bool read_imu_sei(const uint8_t* nal, size_t len, const std::function<void(air::ImuBatch&)>& emit) {
    if (len < 2 + 3) return false;
    int type = (nal[0] >> 1) & 0x3f;
    if (type != 39 && type != 40) return false;

    // remove emulation prevention bytes
    static thread_local std::vector<uint8_t> rb;
    rb.clear();
    rb.reserve(len);
    int zeros = 0;
    for (size_t i = 2; i < len; i++) {
        uint8_t c = nal[i];
        if (zeros >= 2 && c == 3) { zeros = 0; continue; }
        rb.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }

    static thread_local air::ImuBatch b;
    size_t i = 0;
    bool ours = false;
    while (i + 2 <= rb.size() && rb[i] != 0x80) {
        unsigned pt = 0, ps = 0;
        while (i < rb.size() && rb[i] == 0xff) { pt += 255; i++; }
        if (i >= rb.size()) break;
        pt += rb[i++];
        while (i < rb.size() && rb[i] == 0xff) { ps += 255; i++; }
        if (i >= rb.size()) break;
        ps += rb[i++];
        if (i + ps > rb.size()) break;
        const uint8_t* p = rb.data() + i;
        i += ps;
        if (pt != 5 || ps < 16 + 26 || memcmp(p, kUuid, 16) != 0) continue;
        if (p[16] != 1) continue;                      // a version this does not know
        const int n = rd16(p + 16 + 20);
        const double glsb = rd16(p + 16 + 22) / 10.0;  // LSB per degree per second
        const double alsb = rd16(p + 16 + 24);         // LSB per g
        if (glsb <= 0 || alsb <= 0 || (size_t)(16 + 26 + n * 14) > ps) continue;
        ours = true;

        b.message = true;
        b.t0_us = rd64(p + 16 + 12);
        b.gyro_lsb = glsb;
        b.accel_lsb = alsb;
        int k = 0;
        do {                                           // a message with no samples still counts
            b.n = std::min(n - k, air::ImuBatch::kMax);
            for (int j = 0; j < b.n; j++, k++) {
                const uint8_t* s = p + 16 + 26 + k * 14;
                air::ImuBatch::Sample& o = b.s[j];
                o.dt_us = rd16(s);
                for (int a = 0; a < 3; a++) o.gyro[a] = (int16_t)rd16(s + 2 + 2 * a);
                for (int a = 0; a < 3; a++) o.accel[a] = (int16_t)rd16(s + 8 + 2 * a);
            }
            emit(b);
            b.message = false;
        } while (k < n);
    }
    return ours;
}

}  // namespace radio

#pragma once
// The air unit's IMU samples, as kestrel-air --imu sends them: H.265 SEI messages
// (user_data_unregistered, UUID "kestrel-air-IMU1") after the last slice of each
// picture. This turns them into the camera's orientation over time.
//
// Samples carry the air unit's CLOCK_MONOTONIC in microseconds, the same clock as the
// capture stamp in each picture's radio header, so the orientation of the camera at the
// moment a picture was captured is a lookup by that stamp.
//
// Layout of the SEI payload (little-endian), written by kestrel-air src/sky/imu.c:
//   UUID | u8 version 1 | u8 flags | u16 picture | u64 capture PTS | u64 first sample time |
//   u16 n | u16 gyro LSB per 0.1 dps | u16 accel LSB per g | n x { u16 us after the first,
//   i16 gx gy gz ax ay az }
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "quat.hpp"

namespace stab {

class ImuStream {
public:
    static ImuStream& get();

    // One SEI NAL unit, its 2-byte header first, no start code. True if it was the air unit's IMU.
    bool on_sei_nal(const uint8_t* nal, size_t len);

    // The camera's orientation at air-clock time t_us, relative to an arbitrary start:
    // the gyro integrated in the camera's own axes (OpenCV: x right, y down, z forward).
    // False when there is no data around t_us.
    bool orientation_at(int64_t t_us, Quat* q) const;

    // The camera's angular rate, deg/s in its own axes (x right, y down, z forward; the gyro's
    // offset removed), averaged over the newest `window_us` of sample time. y is the pan: positive
    // when the camera turns to the right. False when there is no recent data (none in the last
    // 100 ms of this machine's time).
    bool recent_rate_dps(int64_t window_us, double w_dps[3]) const;

    // Which way is down in the camera's axes (unit vector), while the unit has been still for at
    // least half a second - no turning, and only gravity on the accelerometer. False otherwise,
    // and when there is no recent data.
    bool resting_down(double down[3]) const;

    // A picture's 32-bit capture stamp, extended to the air clock's 64 bits by the newest sample.
    int64_t extend_stamp(uint32_t cap32) const;

    // Camera angle of the air unit, 0 or 180 degrees: it turns the picture, so it turns the
    // gyro's axes too. Changing it starts the orientation over.
    void set_camera_angle(int angle);

    struct Status {
        bool live = false;          // samples arrived in the last second of sample time
        uint64_t samples = 0, messages = 0, resets = 0, gaps = 0;
        double rate_hz = 0;         // over the newest second
        double rate_dps = 0;        // |angular rate| of the newest sample
        double bias_dps[3] = {0, 0, 0};
        int64_t newest_us = 0;
        int64_t age_ms = -1;        // since the last message arrived (this machine's clock); -1 never
    };
    Status status() const;

    // Tests: a fixed gyro bias (dps, IMU axes) instead of the one learned while still.
    void test_fix_bias(const double dps[3]);
    void reset();

private:
    ImuStream() = default;
    struct Entry {
        int64_t t;
        Quat q;
        double w[3];   // camera-frame rate, rad/s
    };
    void add_sample(int64_t t, const double gyro_rad[3], const double accel_g[3]);
    void clear_locked();

    mutable std::mutex mu_;
    std::vector<Entry> ring_ = std::vector<Entry>(4096);
    size_t head_ = 0, count_ = 0;     // newest at (head_ + count_ - 1) % size
    Quat q_;                          // orientation after the newest sample
    int angle_ = 0;
    uint64_t samples_ = 0, messages_ = 0, resets_ = 0, gaps_ = 0;
    int64_t last_rx_ms_ = -1;
    // Resting gravity: the accelerometer in camera axes, averaged over the samples since the unit
    // last moved (no turning, 1 g +- 4%).
    double acc_lp_[3] = {0, 0, 0};
    int64_t still_us_ = 0;
    // bias learning: blocks of still samples
    double bias_[3] = {0, 0, 0};
    bool bias_fixed_ = false;
    double blk_sum_[3] = {0, 0, 0}, blk_sq_ = 0;
    int blk_n_ = 0;

    const Entry& at(size_t i) const { return ring_[(head_ + i) % ring_.size()]; }
};

// The camera's angular rates (x right, y down, z along the lens; deg/s) in the AIRCRAFT's axes, the
// camera being tilted up by tilt_deg on the airframe (negative: down): x the pitch rate, nose up
// positive; y the yaw rate, a right turn positive; z the roll rate, right side down positive. A yaw
// seen by a tilted camera is partly a turn about its lens, and this puts it back.
inline void to_aircraft_rates(const double w[3], double tilt_deg, double out[3]) {
    const double a = tilt_deg * 3.14159265358979323846 / 180.0;
    const double c = __builtin_cos(a), s = __builtin_sin(a);
    out[0] = w[0];
    out[1] = w[1] * c - w[2] * s;
    out[2] = w[1] * s + w[2] * c;
}

}  // namespace stab

#pragma once
// The air unit's IMU (kestrel-air --imu): its samples as each picture brings
// them, and the camera's angle, which turns the gyro's axes with the picture,
// in order with them. One reader.
#include <cstdint>

namespace bus { class Waker; }

namespace air {

struct ImuBatch {
    static constexpr int kMax = 64;
    bool message = false;      // starts one of the air unit's IMU messages (n == 0 and not this: the angle alone)
    int64_t rx_ms = 0;         // when the message arrived, this machine's steady clock
    int camera_angle = 0;      // 0 or 180
    uint64_t t0_us = 0;        // the message's first sample, the air unit's CLOCK_MONOTONIC
    double gyro_lsb = 0;       // LSB per deg/s
    double accel_lsb = 0;      // LSB per g
    int n = 0;                 // samples here (a longer message comes in several batches)
    struct Sample {
        uint16_t dt_us;        // after t0_us
        int16_t gyro[3];       // the IMU's own axes, raw
        int16_t accel[3];
    } s[kMax];
};

class Imu {
public:
    virtual ~Imu() = default;
    virtual bool take(ImuBatch& b) = 0;        // the oldest; false: none
    virtual uint64_t dropped() const = 0;      // that found the queue full
    // Poked for each batch. Set before the threads start; without a watcher
    // the samples are not read out of the stream.
    virtual void watch(bus::Waker* w) = 0;
};

}  // namespace air

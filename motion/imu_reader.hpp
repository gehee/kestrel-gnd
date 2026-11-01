#pragma once
// The reader of the air unit's IMU samples (air::Imu): on a thread of its own
// it feeds them to ImuStream, which the HUD reads. The gyro is integrated as
// the samples come, whether or not anything draws, and off the radio thread:
// the radio only queues each batch.
#include <atomic>
#include <cstdint>
#include <thread>

#include "base/bus.hpp"
#include "system/air/imu.hpp"

namespace motion {

class ImuReader {
public:
    explicit ImuReader(air::Imu& imu);    // watches it and starts its thread: before the radio runs
    ~ImuReader();                          // stops it
    ImuReader(const ImuReader&) = delete;
    ImuReader& operator=(const ImuReader&) = delete;

private:
    void run();

    air::Imu& imu_;
    bus::Waker wake_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace motion

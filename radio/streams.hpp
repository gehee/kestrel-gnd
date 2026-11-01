#pragma once
// The streams a radio carries from the air unit as they come, each to its one
// reader: the FC serial bridge's bytes, with where a new session starts
// (air::Fc), and the air unit's IMU samples, with the camera's angle in order
// with them (air::Imu). The model owns one of each; the radio writes them.
// Nothing here interprets what it carries.
#include <cstddef>
#include <cstdint>

#include "base/bus.hpp"
#include "system/air/fc.hpp"
#include "system/air/imu.hpp"

namespace radio {

// The FC's serial bytes, in chunks, in order. A new session (a new link,
// another aircraft) is a chunk marked screen_reset: the FC's screen starts
// over there. One that finds the queue full is not lost: it goes in first
// when there is room, before newer bytes (the ones in between go). Written
// from any thread; the reader is woken when chunks start coming (2048 hold
// at least the 64 KB the old byte queue did).
class FcFeed : public air::Fc {
public:
    bool watched() const { return wake_ != nullptr; }   // nobody: not worth carrying
    void bytes(const uint8_t* p, size_t n);
    void new_session();

    bool take(air::FcChunk& c) override { return ring_.pop(c); }
    bool pending() const override { return ring_.size() > 0; }
    uint64_t dropped() const override { return ring_.drops(); }
    void watch(bus::Waker* w) override { wake_ = w; }
private:
    bool push(const air::FcChunk& c);   // under mu_; false: full
    bool reset();                       // its chunk, if there was room (under mu_)
    bus::Ring<air::FcChunk, 2048> ring_;
    bus::PiMutex mu_;
    bus::Waker* wake_ = nullptr;
    bool owed_ = false;                 // a reset that found it full (under mu_)
};

// The air unit's IMU (kestrel-air --imu): an SEI from the video stream, its
// samples read out (radio/air_imu) and queued, each batch with the camera's
// angle as the air unit last reported it; each wakes the reader (~1 s of
// them at 120 pictures a second).
class ImuFeed : public air::Imu {
public:
    bool watched() const { return watched_; }
    void sei(const uint8_t* nal, size_t len);   // the radio thread
    void camera_angle(int deg);      // 0 or 180: the picture turns the gyro's axes too

    bool take(air::ImuBatch& b) override { return ring_.pop(b); }
    uint64_t dropped() const override { return ring_.drops(); }
    void watch(bus::Waker* w) override { ring_.set_waker(w); watched_ = w != nullptr; }
private:
    void push(air::ImuBatch& b);
    bus::Ring<air::ImuBatch, 128> ring_;
    bus::PiMutex mu_;
    bool watched_ = false;
    int angle_ = 0;                  // under mu_
};

}  // namespace radio

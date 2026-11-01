#include "radio/streams.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>

#include "radio/air_imu.hpp"

namespace radio {

bool FcFeed::push(const air::FcChunk& c) {
    bool was_empty = false;
    if (!ring_.push(c, &was_empty)) return false;
    if (was_empty && wake_) wake_->poke();
    return true;
}

void FcFeed::bytes(const uint8_t* p, size_t n) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    if (owed_ && !reset()) return;   // still full: these go too
    air::FcChunk c;
    while (n) {
        c.n = (uint16_t)std::min<size_t>(n, air::FcChunk::kMax);
        memcpy(c.b, p, c.n);
        push(c);
        p += c.n;
        n -= c.n;
    }
}

void FcFeed::new_session() {
    std::lock_guard<bus::PiMutex> lk(mu_);
    reset();
}

bool FcFeed::reset() {
    air::FcChunk c;
    c.screen_reset = true;
    const bool landed = push(c);
    owed_ = !landed;
    return landed;
}

void ImuFeed::push(air::ImuBatch& b) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    b.camera_angle = angle_;
    ring_.push(b);                         // full: dropped and counted
}

void ImuFeed::sei(const uint8_t* nal, size_t len) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t rx_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    read_imu_sei(nal, len, [&](air::ImuBatch& b) {
        b.rx_ms = rx_ms;
        push(b);
    });
}

void ImuFeed::camera_angle(int deg) {
    {
        std::lock_guard<bus::PiMutex> lk(mu_);
        angle_ = deg == 180 ? 180 : 0;
    }
    air::ImuBatch b{};                     // the angle alone: no message, no samples
    push(b);
}

}  // namespace radio

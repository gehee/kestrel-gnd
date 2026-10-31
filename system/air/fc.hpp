#pragma once
// The flight controller's serial bytes (MSP), as the air unit relays them, in
// order. One reader.
#include <cstdint>

namespace bus { class Waker; }

namespace air {

// A chunk marked screen_reset starts the FC's screen over (a new link,
// another aircraft) before its bytes.
struct FcChunk {
    static constexpr int kMax = 120;
    bool screen_reset = false;
    uint16_t n = 0;
    uint8_t b[kMax];
};

class Fc {
public:
    virtual ~Fc() = default;
    virtual bool take(FcChunk& c) = 0;         // the oldest; false: none
    virtual bool pending() const = 0;          // some to take
    virtual uint64_t dropped() const = 0;      // that found the queue full
    // Poked when chunks start coming (into an empty queue), not for each: the
    // reader takes them, and looks (pending()) before it sleeps. Set before
    // the threads start; without a watcher the bytes are not carried.
    virtual void watch(bus::Waker* w) = 0;
};

}  // namespace air

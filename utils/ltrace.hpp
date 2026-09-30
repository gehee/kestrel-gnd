#ifndef LTRACE_HPP
#define LTRACE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// Per-frame latency trace, for working out where the time between the air
// unit's camera and the goggle's screen goes.
//
// Off unless asked: `echo 20 > /tmp/ltrace.req` records 20 seconds of events
// into memory and then writes them to /tmp/ltrace.bin, one fixed-size Rec per
// event, CLOCK_MONOTONIC microseconds throughout. Recording costs an atomic
// increment and a 56-byte copy per event, and nothing at all when off.
// tools/ltrace.py reads the file.
namespace ltrace {

enum : uint8_t {
    kRead = 1,    // a: bytes read from the video socket
    kHdr,         // a: air frame counter, b: air capture stamp (us), d: header
    kSlice,       // a: slice address, b: length | first << 32 | nal type << 40
    kAu,          // a: pts, b: first slice arrival - handed to the decoder
    kDecPut,      // a: pts, b: time decode_put_packet returned
    kDecOut,      // a: pts, d: luma of an 8x4 grid of the picture
    kRender,      // a: pts, b: fb id - the frame's flip is being submitted
    kFlipDone,    // a: fb id, b: kernel vblank time of the flip
    kClock = 10,  // b: CLOCK_REALTIME (us) at t - for usbmon, which stamps realtime
};

struct Rec {
    uint64_t t;
    uint8_t  ev;
    uint8_t  n;          // bytes used in d
    uint16_t pad;
    uint32_t a;
    uint64_t b;
    uint8_t  d[32];
};

extern std::atomic<bool> g_on;
inline bool on() { return g_on.load(std::memory_order_relaxed); }

void rec(uint8_t ev, uint64_t t, uint32_t a, uint64_t b,
         const void* d = nullptr, size_t n = 0);
uint64_t now_us();

// Starts the thread that watches for /tmp/ltrace.req. Once, from main().
void start();

}  // namespace ltrace

#endif

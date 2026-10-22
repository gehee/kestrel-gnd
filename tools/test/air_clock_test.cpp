// Host test of utils/frame_clock.hpp's AirClock: the air radio's 32-bit us
// frame stamps across 0xFFFFFFFF -> 0, against our radio's coarse copy.
//   g++ -std=c++17 -I. -pthread -o air_clock_test tools/test/air_clock_test.cpp && ./air_clock_test
#include <cstdio>
#include "utils/frame_clock.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static constexpr uint64_t W = 1ull << 32;   // one wrap, us

// The air's clock at our time t is t + o; our copy reads it +2.2 ms off.
struct Link {
    int64_t o;
    uint32_t s32(uint64_t t) const { return (uint32_t)(t + o); }
    double coarse(uint64_t t) const { return (double)(t + o) + 2200; }
};

int main() {
    // Started (kestrel-gnd) 30 s before the air unit's first wrap, followed
    // through it every 100 ms: the clock goes on past 2^32, never back.
    {
        const Link l{(int64_t)(W - 30000000) - 1000000};
        AirClock a;
        uint64_t t = 1000000;
        CHECK(a.take(l.s32(t), t, l.coarse(t)) == AirClock::kPlaced);
        CHECK(a.us() == t + l.o && a.wraps() == 0);
        bool ok = true;
        for (int i = 0; i < 600; i++) {
            t += 100000;
            ok = ok && a.take(l.s32(t), t, l.coarse(t)) == AirClock::kFollowed && a.us() == t + l.o;
        }
        CHECK(ok);
        CHECK(a.wraps() == 1);
        // ... and on with no coarse copy (not polled yet, or gone).
        t += 100000;
        CHECK(a.take(l.s32(t), t, -1) == AirClock::kFollowed && a.us() == t + l.o);
    }
    // The bug (2026-10-22): kestrel-gnd started after the air unit's 72nd
    // minute. The first stamp is small; the clock is 2^32 + that.
    for (const double off : {2200.0, -2200.0, 750000.0, -750000.0, 4.9e6}) {
        const uint64_t t = 5000000;
        const int64_t o = (int64_t)(W + 123456789) - (int64_t)t;
        AirClock a;
        CHECK(a.take((uint32_t)(t + o), t, (double)(t + o) + off) == AirClock::kPlaced);
        CHECK(a.us() == t + o && a.wraps() == 1);
    }
    // Right at the wrap: a stamp just before 0, our copy just after (and the
    // other way round).
    {
        AirClock a;
        CHECK(a.take(0xFFFFFF00u, 10, (double)(2 * W + 1000)) == AirClock::kPlaced);
        CHECK(a.us() == 2 * W - 256);
        AirClock b;
        CHECK(b.take(0x100u, 10, (double)(2 * W - 1000)) == AirClock::kPlaced);
        CHECK(b.us() == 2 * W + 256);
    }
    // No coarse copy at the first stamp (the AP time not polled yet): taken as
    // is, then put on the copy once there is one.
    {
        const Link l{(int64_t)(3 * W + 777)};
        AirClock a;
        uint64_t t = 2000000;
        CHECK(a.take(l.s32(t), t, -1) == AirClock::kPlaced && a.wraps() == 0);
        t += 100000;
        CHECK(a.take(l.s32(t), t, l.coarse(t)) == AirClock::kCorrected);
        CHECK(a.us() == t + l.o && a.wraps() == 3);
        t += 100000;
        CHECK(a.take(l.s32(t), t, l.coarse(t)) == AirClock::kFollowed && a.us() == t + l.o);
    }
    // A copy that does not agree (another clock): not used.
    {
        AirClock a;
        const uint64_t t = 1000;
        CHECK(a.take(5000000, t, (double)(W + 900000000)) == AirClock::kPlaced && a.us() == 5000000);
    }
    // The air unit started again after 80 min up; our copy still the old
    // clock's: the new clock from 0, not a wrap on.
    {
        const Link old{(int64_t)(W + 8 * 60000000ull)};
        AirClock a;
        uint64_t t = 1000000;
        a.take(old.s32(t), t, old.coarse(t));
        CHECK(a.wraps() == 1);
        t += 100000;
        const uint32_t fresh = 3000000;   // 3 s up
        CHECK(a.take(fresh, t, old.coarse(t)) == AirClock::kStarted);
        CHECK(a.us() == fresh);
        // Our copy catches up with the new clock: on as before.
        t += 100000;
        CHECK(a.take(fresh + 100000, t, (double)fresh + 100000 + 2200) == AirClock::kFollowed);
        CHECK(a.us() == fresh + 100000);
    }
    // Restarted across a wrap with our copy right: the new clock where it says.
    {
        AirClock a;
        a.take(1000, 1000, 1000 + 2200);
        const uint64_t t = 100000000;   // 100 s on, the stamp says 4 ms
        CHECK(a.take(0x1000u, t, (double)(W + 0x1000) + 2200) == AirClock::kStarted);
        CHECK(a.us() == W + 0x1000);
    }
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}

// Unit test of base/bus.hpp: a Topic is never read half-written, a Ring
// keeps order and counts what it drops and a writer that wakes its reader
// only when it was empty loses no wake, pictures go back to their pool when
// the last reference goes, and a Waker's pokes are not lost.
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "base/bus.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Wide { uint32_t v[24]; };   // wider than one word, so a torn read would show

struct Defaults { int a = -1; float b = 2.5f; };

static void topic_test() {
    bus::Topic<Defaults> d;
    Defaults dv{};
    dv.a = 7;
    CHECK(d.read(dv) == 0 && dv.a == -1 && dv.b == 2.5f);   // unpublished: T's defaults
    bus::Topic<Wide> t;
    Wide w{};
    CHECK(t.read(w) == 0);
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::thread reader([&] {
        Wide r;
        while (!stop.load()) {
            t.read(r);
            for (int i = 1; i < 24; i++) if (r.v[i] != r.v[0]) { torn++; break; }
        }
    });
    for (uint32_t n = 1; n <= 2000000; n++) {
        for (auto &x : w.v) x = n;
        t.publish(w);
    }
    stop = true;
    reader.join();
    CHECK(torn.load() == 0);
    CHECK(t.version() == 2000000);
    Wide r;
    t.read(r);
    CHECK(r.v[0] == 2000000 && r.v[23] == 2000000);
}

static void ring_test() {
    bus::Ring<int, 8> r;
    for (int i = 0; i < 10; i++) r.push(i);
    CHECK(r.size() == 8);
    CHECK(r.drops() == 2);
    int v = -1;
    for (int i = 0; i < 8; i++) { CHECK(r.pop(v)); CHECK(v == i); }
    CHECK(!r.pop(v));
    // Across threads: every value arrives once and in order.
    bus::Ring<uint32_t, 256> q;
    std::thread prod([&] { for (uint32_t i = 1; i <= 1000000; ) if (q.push(i)) i++; });
    uint32_t want = 1, got;
    while (want <= 1000000) if (q.pop(got)) { if (got != want) { CHECK(got == want); break; } want++; }
    prod.join();
}

static void pool_test() {
    std::vector<uint8_t> mem(4 * (1000 + 64));
    bus::PicturePool pool(mem.data(), 4, 1000, 64);
    CHECK(pool.free_count() == 4);
    {
        bus::PictureRef a = pool.acquire(), b = pool.acquire(), c = pool.acquire(), d = pool.acquire();
        CHECK(a && b && c && d);
        CHECK(!pool.acquire());                    // all held
        CHECK(pool.empty_count() == 1);
        bus::PictureRef keep = a;                  // a second reference
        a.reset();
        CHECK(pool.free_count() == 0);             // still held by keep
        keep.reset();
        CHECK(pool.free_count() == 1);
        // A view: retain() keeps it, copy_to() counts the copy.
        bus::PictureRef p = pool.acquire();
        std::memcpy(p->data(), "slice", 5);
        p->len = 5;
        bus::SliceView sv{&p, 0, 5, true};
        bus::PictureRef r = sv.retain();
        p.reset();
        CHECK(pool.free_count() == 0);
        uint8_t out[8];
        const uint64_t before = bus::SliceView::copies().load();
        CHECK(sv.len == 5);
        bus::SliceView sv2{&r, 0, 5, true};
        CHECK(sv2.copy_to(out) == 5 && std::memcmp(out, "slice", 5) == 0);
        CHECK(bus::SliceView::copies().load() == before + 5);
    }
    CHECK(pool.free_count() == 4);
    // References dropped on another thread return the buffers too.
    std::vector<bus::PictureRef> held;
    for (int i = 0; i < 4; i++) held.push_back(pool.acquire());
    std::thread t([&] { held.clear(); });
    t.join();
    CHECK(pool.free_count() == 4);
}

// Edge: a push racing the reader's last pop must still wake it (the writer
// pokes only when push() says the ring was empty, as the FC's bytes do). Big
// items make each copy slow enough for the race to happen, the writer pushes
// a second item just as the reader takes the first, and the reader sleeps
// with a long timeout, so a lost wake shows as a long delay.
struct Big { uint64_t sent_us; char pad[8192]; };
static bus::Ring<Big, 4> big_ring;
static void edge_race_test() {
    bus::Ring<Big, 4>& in = big_ring;
    bus::Waker r;
    auto publish = [&](const Big& b) {
        bool was_empty = false;
        if (in.push(b, &was_empty) && was_empty) r.poke();
    };
    std::atomic<bool> done{false};
    std::atomic<uint64_t> worst_us{0}, got{0};
    auto now_us = [] {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    std::thread reader([&] {
        static Big b;
        for (;;) {
            while (in.pop(b)) {
                const uint64_t d = now_us() - b.sent_us;
                if (d > worst_us.load()) worst_us = d;
                got++;
            }
            if (done.load() && !in.size()) break;
            if (!in.size()) r.wait(300000);       // 300 ms: only a wake ends it early
        }
    });
    static Big item;
    uint32_t seed = 1;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < end && worst_us.load() < 100000) {
        item.sent_us = now_us();
        publish(item);
        seed = seed * 1103515245u + 12345u;
        for (volatile uint32_t k = 0, n = (seed >> 16) % 2000; k < n; k++) {}   // the reader's pop, roughly
        item.sent_us = now_us();
        publish(item);
        while (in.size() && worst_us.load() < 100000) {}   // drained, then again
    }
    done = true;
    r.poke();
    reader.join();
    CHECK(got.load() > 1000);
    CHECK(worst_us.load() < 100000);          // never left waiting for the timeout
}

static void waker_test() {
    bus::Waker w;
    w.poke();
    CHECK(w.wait(1000));                       // a poke before the wait is kept
    CHECK(!w.wait(1000));                      // and used once
    std::thread t([&] { usleep(20000); w.poke(); });
    CHECK(w.wait(2000000));
    t.join();
}

int main() {
    topic_test();
    ring_test();
    pool_test();
    edge_race_test();
    waker_test();
    std::printf(fails ? "bus_test: %d FAILED\n" : "bus_test: all passed\n", fails);
    return fails ? 1 : 0;
}

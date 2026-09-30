#include "ltrace.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ltrace {

std::atomic<bool> g_on{false};

static std::vector<Rec> g_buf;
static std::atomic<size_t> g_idx{0};

static void clock_pair() {
    struct timespec m, r;
    clock_gettime(CLOCK_MONOTONIC, &m);
    clock_gettime(CLOCK_REALTIME, &r);
    rec(kClock, (uint64_t)m.tv_sec * 1000000ULL + m.tv_nsec / 1000ULL, 0,
        (uint64_t)r.tv_sec * 1000000ULL + r.tv_nsec / 1000ULL);
}

uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

void rec(uint8_t ev, uint64_t t, uint32_t a, uint64_t b, const void* d, size_t n) {
    if (!on()) return;
    size_t i = g_idx.fetch_add(1, std::memory_order_relaxed);
    if (i >= g_buf.size()) return;
    Rec& r = g_buf[i];
    r.t = t; r.ev = ev; r.a = a; r.b = b; r.pad = 0;
    if (n > sizeof(r.d)) n = sizeof(r.d);
    r.n = (uint8_t)n;
    if (n) memcpy(r.d, d, n);
}

static void run() {
    for (;;) {
        usleep(200000);
        FILE* f = fopen("/tmp/ltrace.req", "r");
        if (!f) continue;
        int secs = 0;
        if (fscanf(f, "%d", &secs) != 1) secs = 10;
        fclose(f);
        unlink("/tmp/ltrace.req");
        if (secs < 1) secs = 1;
        if (secs > 300) secs = 300;

        // Generous: ~2000 events a second at 1080p100 with two slices.
        g_buf.assign((size_t)secs * 8000, Rec{});
        g_idx.store(0);
        printf("ltrace: recording %d s\n", secs);
        g_on.store(true);
        clock_pair();
        sleep(secs);
        clock_pair();
        g_on.store(false);
        usleep(100000);   // let writers that saw g_on finish their copy

        size_t n = g_idx.load();
        if (n > g_buf.size()) n = g_buf.size();
        FILE* o = fopen("/tmp/ltrace.bin.tmp", "wb");
        if (o) {
            fwrite(g_buf.data(), sizeof(Rec), n, o);
            fclose(o);
            rename("/tmp/ltrace.bin.tmp", "/tmp/ltrace.bin");
        }
        printf("ltrace: wrote %zu events to /tmp/ltrace.bin\n", n);
        std::vector<Rec>().swap(g_buf);
    }
}

void start() {
    static_assert(sizeof(Rec) == 56, "Rec layout is read by scripts/ltrace.py");
    std::thread(run).detach();
}

}  // namespace ltrace

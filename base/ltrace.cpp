#include "base/ltrace.hpp"

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
// Writers inside rec() past its first test: run() frees g_buf only once
// none is left, as one that saw g_on may be preempted for any length of time.
static std::atomic<int> g_writers{0};

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
    if (!on()) return;   // off: nothing more than this
    // Counted in, then on looked at again: run() turns it off and then
    // waits for the count, so either it sees this writer or this writer
    // sees it off (both sequentially consistent).
    g_writers.fetch_add(1);
    if (g_on.load()) {
        size_t i = g_idx.fetch_add(1, std::memory_order_relaxed);
        if (i < g_buf.size()) {
            Rec& r = g_buf[i];
            r.t = t; r.ev = ev; r.a = a; r.b = b; r.pad = 0;
            if (n > sizeof(r.d)) n = sizeof(r.d);
            r.n = (uint8_t)n;
            if (n) memcpy(r.d, d, n);
        }
    }
    g_writers.fetch_sub(1);
}

static void run(std::string directory) {
    const auto request = directory + "/ltrace.req";
    const auto output = directory + "/ltrace.bin";
    const auto temporary = output + ".tmp";
    for (;;) {
        usleep(200000);
        FILE* f = fopen(request.c_str(), "r");
        if (!f) continue;
        int secs = 0;
        if (fscanf(f, "%d", &secs) != 1) secs = 10;
        fclose(f);
        unlink(request.c_str());
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
        while (g_writers.load() != 0) usleep(1000);   // writers that saw g_on finish their copy

        size_t n = g_idx.load();
        if (n > g_buf.size()) n = g_buf.size();
        FILE* o = fopen(temporary.c_str(), "wb");
        if (o) {
            fwrite(g_buf.data(), sizeof(Rec), n, o);
            fclose(o);
            rename(temporary.c_str(), output.c_str());
        }
        printf("ltrace: wrote %zu events to %s\n", n, output.c_str());
        std::vector<Rec>().swap(g_buf);
    }
}

void start(const std::string& directory) {
    static_assert(sizeof(Rec) == 56, "Rec layout is read by scripts/ltrace.py");
    std::thread(run, directory).detach();
}

}  // namespace ltrace

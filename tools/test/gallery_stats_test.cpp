// Host test of the stats screen's builder (hud/gallery_stats): the air
// radio's resends (Open8030's resend map) binned by the column of the frame
// they happened in.
//   g++ -std=c++17 -I. -o gallery_stats_test tools/test/gallery_stats_test.cpp hud/gallery_stats.cpp && ./gallery_stats_test
#include "hud/gallery_stats.hpp"
#include <cstdio>
#include <vector>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
    const uint64_t now = 1000000000ULL;                 // a column is 10 s / 240 = 41.6 ms
    std::vector<StatsFrame> frames;
    for (int i = 0; i < 1000; i++) {                    // pictures every 10 ms over the window
        StatsFrame f;
        f.t_us = now - 10000000ULL + (uint64_t)i * 10000;
        f.stage[1] = 5.0f;
        frames.push_back(f);
    }
    const std::vector<StatsFlip> flips;
    std::vector<StatsResend> rs = {
        { now - 20000000ULL, 2 },                       // before the window: not counted
        { now - 5000000ULL, 1 }, { now - 5000000ULL + 3250, 2 },   // one column, 3 blocks
        { now - 1000000ULL, 3 },                        // a stalled frame
        { now + 5000, 1 },                              // after now: not counted
    };
    StatsView v;
    build_stats(frames.data(), frames.size(), flips.data(), 0, now, 10, false, v, 0, rs.data(), rs.size());
    CHECK(v.resent_blocks == 3 && v.resent_frames == 2 && v.stall_frames == 1);
    unsigned cols = 0, stalls = 0, sum = 0;
    for (int c = 0; c < StatsView::kCols; c++) { if (v.resent[c]) cols++; sum += v.resent[c]; stalls += v.stall[c]; }
    CHECK(cols == 1 && sum == 3 && stalls == 1);
    // Without resends: nothing, as before.
    StatsView w;
    build_stats(frames, flips, now, 10, false, w);
    CHECK(w.resent_blocks == 0 && w.stall_frames == 0);
    if (fails) { std::printf("gallery_stats_test: %d failed\n", fails); return 1; }
    std::puts("gallery stats: resends binned by column, window edges, stalled frames");
    return 0;
}

#pragma once

#include <cstdint>
#include <vector>

// What the stats screen draws, worked out from raw samples. Pure data in, pure
// data out: nothing here touches GL or the OSD, so it can be tested off the
// goggle (see gallery_stats.cpp for how a window is cut into columns).

// One entry of the latency history (OSD::add_latency_frame), reduced to what the
// screen needs. t_us is CLOCK_MONOTONIC; entries with every stage at zero are the
// gap fillers the OSD pushes while video is stalled and carry no latency.
struct StatsFrame {
    uint64_t t_us = 0;
    float    stage[6] = {0, 0, 0, 0, 0, 0};   // capture, encode, network, reassemble, decode, display (ms)
    float    video_mbps = 0, link_mbps = 0;
    uint32_t lost = 0;                        // link packets lost, cumulative
    int8_t   snr = 0;
    uint8_t  mcs = 0;
    uint8_t  key = 0;                         // the picture was a keyframe
};

// One display flip: when it completed, and the time since the one before.
struct StatsFlip { uint64_t t_us; uint32_t gap_us; };

struct StatsView {
    static constexpr int kCols = 240;
    static constexpr int kBins = 30;          // 2 ms each; the last takes everything above
    enum Pace : uint8_t { kOnTime = 0, kLate = 1, kStutter = 2, kNoData = 3 };

    int   window_s = 10;
    // The columns sit on a grid fixed to absolute time (a column is col_us wide and
    // starts at a multiple of it), so a sample stays in its column as time goes by and
    // the chart can scroll smoothly by the fraction of a column since grid_us - the
    // start of the newest column, which is still filling and is not to be drawn.
    uint64_t col_us = 1, grid_us = 0;
    bool  any = false;                        // at least one picture in the window
    bool  pacing = false;                     // flip timing means something (vsync on)

    // One entry per column, oldest to newest.
    float    stage[6][kCols];
    float    total[kCols], tmax[kCols];       // mean and worst total latency (ms)
    float    video[kCols], link[kCols];       // Mbps
    float    snr[kCols];
    uint8_t  mcs[kCols], key[kCols], pace[kCols], have[kCols];   // pace: how the flips in the column went (a share of them, see build_stats)
    uint16_t lost[kCols];                     // packets lost in the column
    uint32_t hist[kBins];                     // pictures by total latency

    // Summary of the window.
    float stage_p50[6] = {0, 0, 0, 0, 0, 0};   // median of each stage over the window (ms)
    float p50 = 0, p99 = 0, worst_ms = 0, fps = 0;
    float fps_now = 0;                        // pictures a second over the last two seconds, whatever the window
    int   worst_col = -1;
    float video_now = 0, link_now = 0, snr_now = 0, link_use = 0;
    int   mcs_now = 0;
    uint32_t lost_total = 0;
    // Pacing: a flip is late past 1.6x the window's median gap, a stutter past 2.6x.
    float good_pct = 100, p99_gap_ms = 0, worst_gap_ms = 0;
    int   late = 0, stutters = 0;
};

// frames and flips are oldest first; the arrays may hold more than the window (older entries
// are skipped). Work is one pass over each - no copies, no sorting - because this runs on
// the OSD thread while the video is playing.
void build_stats(const StatsFrame* frames, size_t n_frames, const StatsFlip* flips, size_t n_flips,
                 uint64_t now_us, int window_s, bool pacing_valid, StatsView& out);
inline void build_stats(const std::vector<StatsFrame>& frames, const std::vector<StatsFlip>& flips,
                        uint64_t now_us, int window_s, bool pacing_valid, StatsView& out) {
    build_stats(frames.data(), frames.size(), flips.data(), flips.size(), now_us, window_s, pacing_valid, out);
}

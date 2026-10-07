#pragma once

#include <cstdint>
#include <vector>

// What the stats screen draws, worked out from raw samples. Pure data in, pure
// data out: nothing here touches GL or the OSD, so it can be tested off the
// goggle (see gallery_stats.cpp for how a window is cut into columns).

// One entry of the latency history (OSD::add_latency_frame), reduced to what the
// screen needs. t_us is CLOCK_MONOTONIC; entries with every stage at zero are the
// gap fillers the OSD pushes while video is stalled and carry no latency.
// Each slice's own latency is kept for at most this many slices; a picture
// with more keeps this many, spread from the first to the last.
static constexpr int kLatSlices = 4;

struct StatsFrame {
    static constexpr int kStages = 4;         // the slowest slice's: encode, rf, decode, display
    uint64_t t_us = 0;
    float    stage[kStages] = {0, 0, 0, 0};   // ms
    float    video_mbps = 0, link_mbps = 0;
    uint32_t lost = 0;                        // link packets lost, cumulative
    int8_t   snr = 0;
    uint8_t  mcs = 0;
    uint8_t  key = 0;                         // the picture was a keyframe
    uint16_t skipped = 0;                     // pictures before it never shown
    uint8_t  nslices = 0;                     // slices timed on their own (0: none), top to bottom
    float    slice_ms[kLatSlices] = {0, 0, 0, 0};
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
    float    stage[StatsFrame::kStages][kCols];
    float    total[kCols], tmax[kCols];       // mean and worst total latency (ms)
    float    kmax[kCols];                     // the worst keyframe's total latency in the column (0: none)
    float    video[kCols], link[kCols];       // Mbps
    float    snr[kCols];
    uint8_t  mcs[kCols], key[kCols], pace[kCols], have[kCols];   // pace: how the flips in the column went (a share of them, see build_stats)
    uint16_t lost[kCols];                     // packets lost in the column
    uint32_t hist[kBins];                     // pictures by total latency

    // Summary of the window.
    float stage_p50[StatsFrame::kStages] = {0, 0, 0, 0};   // median of each stage over the window (ms)
    float p50 = 0, p99 = 0, worst_ms = 0, fps = 0;
    int   nslices = 0;                        // each slice's median, top to bottom (0: not timed so)
    float slice_p50[kLatSlices] = {0, 0, 0, 0};
    float fps_now = 0;                        // pictures a second over the last two seconds, whatever the window
    int   worst_col = -1;
    float video_now = 0, link_now = 0, snr_now = 0, link_use = 0;
    int   mcs_now = 0;
    int   freq_mhz = 0, bw_idx = -1;          // the link's channel and bandwidth gear now (0 / -1: no link)
    float vtx_temp_c = -1;                    // the air unit's SoC temperature now (cmd 0x05), -1: not known
    float vrx_temp_c = -1;                    // this goggle's SoC temperature now, -1: not known
    uint32_t lost_total = 0;
    // Pacing, as the motion is seen: each picture's latency against the fastest
    // of the half second before it (its motion's lateness against the camera's).
    // The screen's refresh quantises latency by up to one refresh, so on time
    // is within one refresh of that (plus 1 ms of radio jitter), late within
    // two, a stutter beyond - and each picture never shown is a stutter. A
    // video rate that does not divide the refresh rate (100 fps on 120 Hz) is
    // on time; a picture held a refresh longer than it had to be is late.
    float good_pct = 100, p99_gap_ms = 0, worst_gap_ms = 0;   // lateness above that fastest, ms
    int   late = 0, stutters = 0;
};

// frames and flips are oldest first; the arrays may hold more than the window (older entries
// are skipped). Work is one pass over each - no copies, no sorting - because this runs on
// the OSD thread while the video is playing.
// period_us: the screen's refresh, for the pacing (0: taken as 1/60 s).
void build_stats(const StatsFrame* frames, size_t n_frames, const StatsFlip* flips, size_t n_flips,
                 uint64_t now_us, int window_s, bool pacing_valid, StatsView& out, double period_us = 0);
inline void build_stats(const std::vector<StatsFrame>& frames, const std::vector<StatsFlip>& flips,
                        uint64_t now_us, int window_s, bool pacing_valid, StatsView& out, double period_us = 0) {
    build_stats(frames.data(), frames.size(), flips.data(), flips.size(), now_us, window_s, pacing_valid, out, period_us);
}

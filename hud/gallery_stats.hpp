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

// One slice's way through, for the life-of-a-slice chart (ms): off is its first
// row's capture after the picture's (row 0), then its stages, each with the
// share of it spent waiting (within it), and Enc's share that is the sensor
// reading the slice's rows (enc_read, within it; 0: not known), and the
// exposure that ends at its first row's capture (exp, before it; 0: not known).
struct SliceStages {
    float off = 0, enc = 0, rf = 0, dec = 0, dec_wait = 0, disp = 0, disp_wait = 0, enc_read = 0, exp = 0;
};

struct StatsFrame {
    static constexpr int kStages = 4;         // the slowest slice's: encode, rf, decode, display
    uint64_t t_us = 0;
    float    stage[kStages] = {0, 0, 0, 0};   // ms
    float    wait[kStages] = {0, 0, 0, 0};    // each stage's share spent waiting (ms, within stage)
    float    video_mbps = 0, link_mbps = 0;
    uint32_t lost = 0;                        // link packets lost, cumulative
    int8_t   snr = 0;
    uint8_t  mcs = 0;
    int8_t   bw = -1;                         // the link's bandwidth gear (0..5 = 1.25 .. 40 MHz), -1: not known
    uint8_t  air = 1;                         // its air part known (0: the radio clock not checked - counted with its air part at 0)
    uint8_t  key = 0;                         // the picture was a keyframe
    uint16_t skipped = 0;                     // pictures before it never shown
    uint64_t shown_us = 0, cap_us = 0;        // its top first up (the vblank), its capture (0: not known)
    uint8_t  nslices = 0;                     // slices timed on their own (0: none), top to bottom
    float    slice_ms[kLatSlices] = {0, 0, 0, 0};
    SliceStages sl[kLatSlices];               // each timed slice's stages (nslices of them)
};

// One display flip: when it completed, and the time since the one before.
struct StatsFlip { uint64_t t_us; uint32_t gap_us; };

struct StatsView {
    static constexpr int kCols = 240;
    static constexpr int kBins = 30;          // 2 ms each; the last takes everything above
    // Frame spacing (see build_stats): how long after the picture before each
    // one went up, in ranges around the source's own spacing S (its captures'):
    // under S-1.5 ms, to S-0.3, S +- 0.3 (on target), to S+1.5, to 1.5 S,
    // about 2 S, about 3 S, over 3.5 S.
    static constexpr int kSpBins = 8, kSpTarget = 2;

    int   window_s = 10;
    // The columns sit on a grid fixed to absolute time (a column is col_us wide and
    // starts at a multiple of it), so a sample stays in its column as time goes by and
    // the chart can scroll smoothly by the fraction of a column since grid_us - the
    // start of the newest column, which is still filling and is not to be drawn.
    uint64_t col_us = 1, grid_us = 0;
    bool  any = false;                        // at least one picture in the window
    bool  pacing = false;                     // the spacing means something (vsync on, pictures timed)

    // One entry per column, oldest to newest.
    float    stage[StatsFrame::kStages][kCols];
    float    wait[StatsFrame::kStages][kCols];   // the waiting share of each stage (within it)
    float    total[kCols], tmax[kCols];       // mean and worst total latency (ms)
    float    kmax[kCols];                     // the worst keyframe's total latency in the column (0: none)
    float    video[kCols], link[kCols];       // Mbps
    float    snr[kCols];
    float    bw_mhz[kCols];                   // the link's bandwidth, MHz (0: not known)
    uint8_t  mcs[kCols], key[kCols], have[kCols];
    uint16_t skip[kCols];                     // pictures in the column never shown
    uint16_t lost[kCols];                     // packets lost in the column
    uint32_t hist[kBins];                     // pictures by total latency

    // Summary of the window.
    float stage_p50[StatsFrame::kStages] = {0, 0, 0, 0};   // median of each stage over the window (ms)
    // Each stage's waiting share and the rest (active), medians over the window
    // like stage_p50 (medians: they do not add up exactly to the total's).
    float wait_p50[StatsFrame::kStages] = {0, 0, 0, 0};
    float active_p50[StatsFrame::kStages] = {0, 0, 0, 0};
    float p50 = 0, p99 = 0, worst_ms = 0, fps = 0;
    int   nslices = 0;                        // each slice's median, top to bottom (0: not timed so)
    float slice_p50[kLatSlices] = {0, 0, 0, 0};
    float slice_p99[kLatSlices] = {0, 0, 0, 0};   // and its p99
    SliceStages sl_p50[kLatSlices];           // each slice's stages, medians (nslices of them)
    float fps_now = 0;                        // pictures shown a second over the last two seconds, whatever the window
    float fps_dec_now = 0;                    // and decoded: those shown and those never shown (overtaken)
    int   worst_col = -1;
    float video_now = 0, link_now = 0, snr_now = 0, link_use = 0;
    int   mcs_now = 0;
    int   freq_mhz = 0, bw_idx = -1;          // the link's channel and bandwidth gear now (0 / -1: no link)
    float bw_mhz_max = 0;                     // the widest in the window, MHz (0: none known)
    float vtx_temp_c = -1;                    // the air unit's SoC temperature now (cmd 0x05), -1: not known
    float vrx_temp_c = -1;                    // this goggle's SoC temperature now, -1: not known
    uint32_t lost_total = 0;
    // Frame spacing: pictures shown by range (kSpBins), how many, the source's
    // spacing S (ms; 0: not known) and the share on target, percent.
    uint32_t sp[kSpBins] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t sp_n = 0;
    float sp_src_ms = 0, on_target_pct = 0;
    uint32_t skipped = 0;                     // pictures never shown in the window
    float refresh_ms = 0;                     // the screen's refresh (0: not known), for the slice timelines' vblanks
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

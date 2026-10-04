#include "gallery_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// The value below which a fraction q of the n samples in hist (bins of `width`) lie, to
// within a bin: the centre of the bin the q-th sample falls in. n must not be zero.
float hist_pct(const uint32_t* hist, int bins, float width, uint64_t n, double q) {
    uint64_t target = (uint64_t)((double)n * q);
    if (target >= n) target = n - 1;
    uint64_t seen = 0;
    for (int i = 0; i < bins; i++) {
        seen += hist[i];
        if (seen > target) return ((float)i + 0.5f) * width;
    }
    return ((float)bins - 0.5f) * width;
}

constexpr int kTotBins = 2001;      // 0.1 ms, to 200 ms (the last takes the rest)
constexpr int kStageBins = 1001;    // 0.1 ms, to 100 ms
constexpr int kGapBins = 4001;      // 50 us, to 200 ms

float total_of(const StatsFrame& f) {
    float t = 0;
    for (float s : f.stage) t += s;
    return t;
}

} // namespace

// The window is the last `window_s` seconds up to now, cut into kCols equal
// columns on a grid anchored to absolute time (see StatsView::col_us). A column
// holds the mean of what fell in it (and the worst, for latency and flip gaps,
// because a spike averaged over a column all but disappears). The last column is
// the one still filling.
void build_stats(const StatsFrame* frames, size_t n_frames, const StatsFlip* flips, size_t n_flips,
                 uint64_t now_us, int window_s, bool pacing_valid, StatsView& out) {
    out = StatsView();
    out.window_s = window_s;
    out.pacing = pacing_valid;
    std::memset(out.pace, StatsView::kNoData, sizeof(out.pace));

    const uint64_t win_us = (uint64_t)window_s * 1000000ULL;
    const uint64_t col_us = win_us / StatsView::kCols;
    const uint64_t gn = now_us / col_us;                         // the newest column's place on the grid
    const uint64_t g0 = gn >= (uint64_t)StatsView::kCols - 1 ? gn - (StatsView::kCols - 1) : 0;
    const uint64_t t0 = g0 * col_us;
    out.col_us = col_us;
    out.grid_us = gn * col_us;
    auto col_of = [&](uint64_t t) {
        if (t < t0) return -1;
        const uint64_t c = t / col_us - g0;
        return c >= (uint64_t)StatsView::kCols ? StatsView::kCols - 1 : (int)c;
    };

    // ---- pictures ----
    int   n[StatsView::kCols] = {0};
    float st_sum[6][StatsView::kCols] = {{0}};
    float v_sum[StatsView::kCols] = {0}, l_sum[StatsView::kCols] = {0}, s_sum[StatsView::kCols] = {0};
    static thread_local uint32_t tot_hist[kTotBins];
    static thread_local uint32_t stage_hist[6][kStageBins];
    std::memset(tot_hist, 0, sizeof(tot_hist));
    std::memset(stage_hist, 0, sizeof(stage_hist));
    uint64_t n_pics = 0, n_recent = 0;
    uint64_t stage_nonzero[6] = {0, 0, 0, 0, 0, 0};
    uint32_t prev_lost = 0;
    bool have_prev = false;
    double v_all = 0, l_all = 0;
    size_t l_n = 0;
    uint64_t first_t = 0;
    for (size_t fi = 0; fi < n_frames; fi++) {
        const StatsFrame& f = frames[fi];
        // The packet-loss counter is cumulative; a column gets what it grew by.
        const bool counted = have_prev && f.lost >= prev_lost;
        const uint32_t grew = counted ? f.lost - prev_lost : 0;
        prev_lost = f.lost;
        have_prev = true;
        const int c = col_of(f.t_us);
        if (c < 0) continue;
        const float tot = total_of(f);
        if (grew) {
            out.lost[c] = (uint16_t)std::min<uint32_t>(65535, out.lost[c] + grew);
            out.lost_total += grew;
        }
        if (tot <= 0.0f) continue;                  // a gap filler: no picture
        if (!first_t) first_t = f.t_us;
        n[c]++;
        for (int k = 0; k < 6; k++) st_sum[k][c] += f.stage[k];
        v_sum[c] += f.video_mbps;
        l_sum[c] += f.link_mbps;
        s_sum[c] += f.snr;
        out.tmax[c] = std::max(out.tmax[c], tot);
        out.key[c] |= f.key;
        out.mcs[c] = f.mcs;
        n_pics++;
        if (f.t_us + 2000000 > now_us) n_recent++;
        tot_hist[std::min(kTotBins - 1, (int)(tot * 10.0f))]++;
        for (int k = 0; k < 6; k++) {
            stage_hist[k][std::min(kStageBins - 1, (int)(f.stage[k] * 10.0f))]++;
            if (f.stage[k] > 0.0f) stage_nonzero[k]++;
        }
        if (tot > out.worst_ms) out.worst_ms = tot;
        v_all += f.video_mbps;
        if (f.link_mbps > 0) { l_all += f.link_mbps; l_n++; }
        out.video_now = f.video_mbps;
        out.link_now = f.link_mbps;
        out.snr_now = f.snr;
        out.mcs_now = f.mcs;
    }
    for (int c = 0; c < StatsView::kCols; c++) {
        if (!n[c]) continue;
        out.have[c] = 1;
        const float inv = 1.0f / (float)n[c];
        float tot = 0;
        for (int k = 0; k < 6; k++) { out.stage[k][c] = st_sum[k][c] * inv; tot += out.stage[k][c]; }
        out.total[c] = tot;
        out.video[c] = v_sum[c] * inv;
        out.link[c] = l_sum[c] * inv;
        out.snr[c] = s_sum[c] * inv;
    }
    if (n_pics) {
        out.any = true;
        out.p50 = hist_pct(tot_hist, kTotBins, 0.1f, n_pics, 0.5);
        out.p99 = hist_pct(tot_hist, kTotBins, 0.1f, n_pics, 0.99);
        // A stage the link never reports is exactly zero, not the middle of the lowest bin.
        for (int k = 0; k < 6; k++)
            out.stage_p50[k] = stage_nonzero[k] ? hist_pct(stage_hist[k], kStageBins, 0.1f, n_pics, 0.5) : 0.0f;
        // The 2 ms bins of the screen's histogram are 20 of the fine ones.
        for (int b = 0; b < kTotBins; b++)
            if (tot_hist[b]) out.hist[std::min((b / 20), StatsView::kBins - 1)] += tot_hist[b];
        for (int c = 0; c < StatsView::kCols - 1; c++)       // not the column still filling
            if (out.have[c] && (out.worst_col < 0 || out.tmax[c] > out.tmax[out.worst_col])) out.worst_col = c;
        // The history can be shorter than the window; the rate is over what there is.
        const double span_s = std::max(1.0, std::min((double)window_s, (double)(now_us - first_t) / 1e6));
        out.fps = (float)((double)n_pics / span_s);
        out.fps_now = (float)((double)n_recent / std::min(2.0, span_s));
        if (l_n) out.link_use = (float)(100.0 * (v_all / (double)n_pics) / (l_all / (double)l_n));
    }

    // ---- flips ----
    if (!pacing_valid) { out.good_pct = 0; return; }
    static thread_local uint32_t gap_hist[kGapBins];
    std::memset(gap_hist, 0, sizeof(gap_hist));
    uint64_t n_gaps = 0;
    for (size_t i = 0; i < n_flips; i++) {
        const StatsFlip& f = flips[i];
        if (f.t_us < t0 || f.gap_us == 0) continue;
        gap_hist[std::min(kGapBins - 1, (int)(f.gap_us / 50))]++;
        n_gaps++;
    }
    if (n_gaps < 10) { out.pacing = false; out.good_pct = 0; return; }
    const float med = std::max(1.0f, hist_pct(gap_hist, kGapBins, 50.0f, n_gaps, 0.5));   // us
    // Per column: how many flips, and how many of them late / stuttering.
    static thread_local uint16_t col_n[StatsView::kCols], col_late[StatsView::kCols], col_stut[StatsView::kCols];
    std::memset(col_n, 0, sizeof(col_n)); std::memset(col_late, 0, sizeof(col_late)); std::memset(col_stut, 0, sizeof(col_stut));
    out.p99_gap_ms = hist_pct(gap_hist, kGapBins, 50.0f, n_gaps, 0.99) / 1000.0f;
    int ok = 0;
    for (size_t i = 0; i < n_flips; i++) {
        const StatsFlip& f = flips[i];
        if (f.t_us < t0 || f.gap_us == 0) continue;
        const float g = (float)f.gap_us;
        const uint8_t cls = g > 2.6f * med ? StatsView::kStutter : g > 1.6f * med ? StatsView::kLate : StatsView::kOnTime;
        out.worst_gap_ms = std::max(out.worst_gap_ms, g / 1000.0f);
        if (cls == StatsView::kOnTime) ok++;
        else if (cls == StatsView::kLate) out.late++;
        else out.stutters++;
        const int c = col_of(f.t_us);
        if (c < 0) continue;
        if (col_n[c] < 65535) col_n[c]++;
        if (cls == StatsView::kLate && col_late[c] < 65535) col_late[c]++;
        if (cls == StatsView::kStutter && col_stut[c] < 65535) col_stut[c]++;
    }
    // A column is coloured by how much of it was bad, so the same figure means the same
    // whether it is 40 ms wide (a handful of flips: one stutter is red) or 750 ms (about
    // seventy: one stutter is amber, a few are red). A column with no flips stays no-data.
    for (int c = 0; c < StatsView::kCols; c++) {
        if (!col_n[c]) continue;
        const float n = (float)col_n[c];
        out.pace[c] = (float)col_stut[c] >= 0.04f * n ? StatsView::kStutter
                    : (col_stut[c] > 0 || (float)col_late[c] >= 0.10f * n) ? StatsView::kLate
                    : StatsView::kOnTime;
    }
    out.good_pct = 100.0f * (float)ok / (float)n_gaps;
}

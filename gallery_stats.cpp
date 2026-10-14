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
constexpr int kCapBins = 5001;      // 10 us, to 50 ms

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
                 uint64_t now_us, int window_s, bool pacing_valid, StatsView& out, double period_us) {
    (void)flips; (void)n_flips;
    out = StatsView();
    out.window_s = window_s;
    out.pacing = pacing_valid;
    out.refresh_ms = period_us > 1000 ? (float)(period_us / 1000.0) : 0.0f;
    std::memset(out.skip, 0, sizeof(out.skip));

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
    constexpr int S = StatsFrame::kStages;
    int   n[StatsView::kCols] = {0};
    float st_sum[S][StatsView::kCols] = {{0}};
    float wt_sum[S][StatsView::kCols] = {{0}};

    float v_sum[StatsView::kCols] = {0}, l_sum[StatsView::kCols] = {0}, s_sum[StatsView::kCols] = {0};
    static thread_local uint32_t tot_hist[kTotBins];
    static thread_local uint32_t slice_hist[kLatSlices][kTotBins];
    // each slice's stages, field by field (SliceStages, 7 floats)
    constexpr int kSlF = 9;
    static thread_local uint32_t sl_hist[kLatSlices][kSlF][kStageBins];
    std::memset(sl_hist, 0, sizeof(sl_hist));
    uint32_t n_sliced = 0;
    // The newest picture's number of slices: only pictures cut the same way count.
    const int ns = n_frames ? frames[n_frames - 1].nslices : 0;
    static thread_local uint32_t stage_hist[S][kStageBins];
    static thread_local uint32_t wait_hist[S][kStageBins], active_hist[S][kStageBins];
    std::memset(tot_hist, 0, sizeof(tot_hist));
    std::memset(slice_hist, 0, sizeof(slice_hist));
    std::memset(stage_hist, 0, sizeof(stage_hist));
    std::memset(wait_hist, 0, sizeof(wait_hist));
    std::memset(active_hist, 0, sizeof(active_hist));
    uint64_t n_pics = 0, n_recent = 0, skipped_recent = 0;
    uint64_t stage_nonzero[S] = {0, 0, 0, 0};
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
        if (f.skipped) {
            out.skip[c] = (uint16_t)std::min<uint32_t>(65535, out.skip[c] + f.skipped);
            out.skipped += f.skipped;
        }
        if (tot <= 0.0f) continue;                  // a gap filler: no picture
        if (!first_t) first_t = f.t_us;
        n[c]++;
        for (int k = 0; k < S; k++) {
            st_sum[k][c] += f.stage[k];
            wt_sum[k][c] += f.wait[k];
        }
        v_sum[c] += f.video_mbps;
        l_sum[c] += f.link_mbps;
        s_sum[c] += f.snr;
        out.tmax[c] = std::max(out.tmax[c], tot);
        if (f.key) out.kmax[c] = std::max(out.kmax[c], tot);
        out.key[c] |= f.key;
        out.mcs[c] = f.mcs;
        // The newest in the column, like the MCS: a change is a step.
        if (f.bw >= 0 && f.bw <= 5) {
            static const float kMhz[6] = { 1.25f, 2.5f, 5, 10, 20, 40 };
            out.bw_mhz[c] = kMhz[f.bw];
            out.bw_mhz_max = std::max(out.bw_mhz_max, kMhz[f.bw]);
        }
        n_pics++;
        if (f.t_us + 2000000 > now_us) { n_recent++; skipped_recent += f.skipped; }
        tot_hist[std::min(kTotBins - 1, (int)(tot * 10.0f))]++;
        if (ns >= 2 && f.nslices == ns) {
            for (int s = 0; s < ns; s++) {
                slice_hist[s][std::min(kTotBins - 1, (int)(f.slice_ms[s] * 10.0f))]++;                const float v[kSlF] = { f.sl[s].off, f.sl[s].enc, f.sl[s].rf, f.sl[s].dec,
                                        f.sl[s].dec_wait, f.sl[s].disp, f.sl[s].disp_wait, f.sl[s].enc_read,
                                        f.sl[s].exp };
                for (int q = 0; q < kSlF; q++)
                    sl_hist[s][q][std::max(0, std::min(kStageBins - 1, (int)(v[q] * 10.0f)))]++;
            }
            n_sliced++;
        }
        for (int k = 0; k < S; k++) {
            stage_hist[k][std::min(kStageBins - 1, (int)(f.stage[k] * 10.0f))]++;
            const float w = std::min(f.wait[k], f.stage[k]);
            wait_hist[k][std::min(kStageBins - 1, (int)(w * 10.0f))]++;
            active_hist[k][std::min(kStageBins - 1, (int)((f.stage[k] - w) * 10.0f))]++;
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
        for (int k = 0; k < S; k++) {
            out.stage[k][c] = st_sum[k][c] * inv;
            out.wait[k][c] = wt_sum[k][c] * inv;
            tot += out.stage[k][c];
        }
        out.total[c] = tot;
        out.video[c] = v_sum[c] * inv;
        out.link[c] = l_sum[c] * inv;
        out.snr[c] = s_sum[c] * inv;
    }
    if (n_pics) {
        out.any = true;
        out.p50 = hist_pct(tot_hist, kTotBins, 0.1f, n_pics, 0.5);
        out.p99 = hist_pct(tot_hist, kTotBins, 0.1f, n_pics, 0.99);
        if (n_sliced) {
            out.nslices = ns;
            // Each slice's stages as the typical picture's: the mean of each
            // stage over the pictures whose total is in the middle fifth of
            // that slice's (p40..p60). Stage by stage medians do not add up
            // to the median total - in SPLIT the top's came out 1 ms short of
            // it - while these do, and keep the outliers out all the same.
            float lo[kLatSlices], hi[kLatSlices];
            double sum[kLatSlices][kSlF] = {{0}};
            uint32_t cnt[kLatSlices] = {0};
            for (int s = 0; s < ns; s++) {
                out.slice_p50[s] = hist_pct(slice_hist[s], kTotBins, 0.1f, n_sliced, 0.5);
                out.slice_p99[s] = hist_pct(slice_hist[s], kTotBins, 0.1f, n_sliced, 0.99);
                lo[s] = hist_pct(slice_hist[s], kTotBins, 0.1f, n_sliced, 0.4) - 0.05f;
                hi[s] = hist_pct(slice_hist[s], kTotBins, 0.1f, n_sliced, 0.6) + 0.05f;
            }
            for (size_t fi = 0; fi < n_frames; fi++) {
                const StatsFrame& f = frames[fi];
                if (col_of(f.t_us) < 0 || total_of(f) <= 0.0f || f.nslices != ns) continue;
                for (int s = 0; s < ns; s++) {
                    if (f.slice_ms[s] < lo[s] || f.slice_ms[s] > hi[s]) continue;
                    const float v[kSlF] = { f.sl[s].off, f.sl[s].enc, f.sl[s].rf, f.sl[s].dec,
                                            f.sl[s].dec_wait, f.sl[s].disp, f.sl[s].disp_wait, f.sl[s].enc_read,
                                            f.sl[s].exp };
                    for (int q = 0; q < kSlF; q++) sum[s][q] += v[q];
                    cnt[s]++;
                }
            }
            for (int s = 0; s < ns; s++) {
                float* o[kSlF] = { &out.sl_p50[s].off, &out.sl_p50[s].enc, &out.sl_p50[s].rf,
                                   &out.sl_p50[s].dec, &out.sl_p50[s].dec_wait,
                                   &out.sl_p50[s].disp, &out.sl_p50[s].disp_wait, &out.sl_p50[s].enc_read,
                                   &out.sl_p50[s].exp };
                for (int q = 0; q < kSlF; q++)
                    *o[q] = cnt[s] ? (float)(sum[s][q] / cnt[s])
                                   : hist_pct(sl_hist[s][q], kStageBins, 0.1f, n_sliced, 0.5);
            }
        }
        // A stage the link never reports is exactly zero, not the middle of the lowest bin.
        for (int k = 0; k < S; k++) {
            out.stage_p50[k] = stage_nonzero[k] ? hist_pct(stage_hist[k], kStageBins, 0.1f, n_pics, 0.5) : 0.0f;
            out.wait_p50[k] = stage_nonzero[k] ? hist_pct(wait_hist[k], kStageBins, 0.1f, n_pics, 0.5) : 0.0f;
            out.active_p50[k] = stage_nonzero[k] ? hist_pct(active_hist[k], kStageBins, 0.1f, n_pics, 0.5) : 0.0f;
        }
        // The 2 ms bins of the screen's histogram are 20 of the fine ones.
        for (int b = 0; b < kTotBins; b++)
            if (tot_hist[b]) out.hist[std::min((b / 20), StatsView::kBins - 1)] += tot_hist[b];
        for (int c = 0; c < StatsView::kCols - 1; c++)       // not the column still filling
            if (out.have[c] && (out.worst_col < 0 || out.tmax[c] > out.tmax[out.worst_col])) out.worst_col = c;
        // The history can be shorter than the window; the rate is over what there is.
        const double span_s = std::max(1.0, std::min((double)window_s, (double)(now_us - first_t) / 1e6));
        out.fps = (float)((double)n_pics / span_s);
        out.fps_now = (float)((double)n_recent / std::min(2.0, span_s));
        out.fps_dec_now = (float)((double)(n_recent + skipped_recent) / std::min(2.0, span_s));
        if (l_n) out.link_use = (float)(100.0 * (v_all / (double)n_pics) / (l_all / (double)l_n));
    }

    // ---- frame spacing: each picture against the one shown before it ----
    // How long after the picture before it each one went up (the vblanks that
    // first put their tops up), against the source's spacing S: the median gap
    // between captures, so the camera's real rate (a screen lock's 120.19).
    // On target is S +- 0.3 ms. A screen at the source's rate puts every
    // picture one refresh after the last; a picture held a refresh longer (the
    // next late, or overtaken) shows as about 2 S; a source rate the screen
    // does not match (100 fps on 120 Hz) lands off target by design - that is
    // its judder. A video stall is one long gap.
    if (!pacing_valid) return;
    static thread_local uint32_t cap_hist[kCapBins];      // capture gaps, 10 us bins
    std::memset(cap_hist, 0, sizeof(cap_hist));
    uint64_t prev_cap = 0, n_cap = 0;
    for (size_t fi = 0; fi < n_frames; fi++) {
        const StatsFrame& f = frames[fi];
        if (f.t_us < t0 || !f.cap_us || total_of(f) <= 0.0f) { prev_cap = 0; continue; }
        if (prev_cap && f.cap_us > prev_cap) {
            cap_hist[std::min<uint64_t>(kCapBins - 1, (f.cap_us - prev_cap) / 10)]++;
            n_cap++;
        }
        prev_cap = f.cap_us;
    }
    if (n_cap < 10) { out.pacing = false; return; }
    const float src = hist_pct(cap_hist, kCapBins, 10.0f, n_cap, 0.5) / 1000.0f;   // S, ms
    out.sp_src_ms = src;
    uint64_t prev_shown = 0;
    for (size_t fi = 0; fi < n_frames; fi++) {
        const StatsFrame& f = frames[fi];
        if (f.t_us < t0 || !f.shown_us || total_of(f) <= 0.0f) continue;
        if (prev_shown && f.shown_us > prev_shown) {
            const float d = (float)(f.shown_us - prev_shown) / 1000.0f;
            const int k = d < src - 1.5f ? 0 : d < src - 0.3f ? 1 : d <= src + 0.3f ? 2 : d <= src + 1.5f ? 3
                        : d <= 1.5f * src ? 4 : d <= 2.5f * src ? 5 : d <= 3.5f * src ? 6 : 7;
            out.sp[k]++;
            out.sp_n++;
        }
        if (f.shown_us > prev_shown) prev_shown = f.shown_us;
    }
    if (out.sp_n < 10) { out.pacing = false; return; }
    out.on_target_pct = 100.0f * (float)out.sp[StatsView::kSpTarget] / (float)out.sp_n;
}

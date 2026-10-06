#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

// One picture's slices, each timed on its own from the moment its first row
// was captured to the moment that row is lit on the screen - no slice waits
// for another, so nothing here is "the picture's". Shared by the stages that
// fill it in: the AR8030 source (rows, capture, encoder out, arrival), the
// decoder (when each slice's rows are decoded) and the flip that shows it
// (the vblank, see DrmDevice::take_slice_flips). All on CLOCK_MONOTONIC, us;
// 0 = not (yet) known.
struct SliceTimes {
    static constexpr int kMax = 8;
    uint32_t height = 0;                       // picture rows
    uint64_t frame_cap_us = 0;                 // the capture stamp: row 0
    bool key = false;                          // a keyframe
    int64_t pts = -1;                          // the decoder's picture number
    std::atomic<bool> counted{false};          // its latency sample is taken (once per picture)
    int expected = 0;                          // slices a picture had, as learned when it started
    std::atomic<bool> complete{false};         // its last slice is in (or none will come)
    // Where the first slice ends, row, before the second has come to say so:
    // learned by the source (a picture that restarts the intra refresh is cut
    // higher). 0 = not known. Once there, the second slice's own row counts.
    std::atomic<uint32_t> split_row{0};
    std::atomic<int> n{0};                     // slices so far
    struct Slice {
        uint32_t row = 0;                      // its first row
        uint64_t cap_us = 0;                   // its first row captured
        uint64_t out_us = 0;                   // handed out by the air unit's encoder
        uint64_t here_us = 0;                  // received here
        std::atomic<uint64_t> done_us{0};      // its rows decoded
    } s[kMax];
    // The flips that put rows of this picture up: rows [from, to) from vblank
    // on. Kept as rows, not slices, so a slice that arrives after its rows
    // went up (decoded in place, in the buffer on screen) still finds them.
    // One writer: the thread that sees the flips land.
    struct Shown { uint32_t from, to; uint64_t vblank; };
    static constexpr int kShows = 6;
    Shown shows[kShows];
    std::atomic<int> nshows{0};
    // When the scan-out first had row's picture to show: 0 if never.
    uint64_t lit_vblank(uint32_t row) const {
        const int k = nshows.load(std::memory_order_acquire);
        uint64_t v = 0;
        for (int i = 0; i < k; i++)
            if (row >= shows[i].from && row < shows[i].to && (!v || shows[i].vblank < v)) v = shows[i].vblank;
        return v;
    }
    // The row the first slice ends at, as far as is known now (0: not known).
    uint32_t first_end() const {
        return n.load() >= 2 ? s[1].row : split_row.load();
    }
    // A flip that landed at vblank_us showed this picture's rows [from, to).
    void shown(uint64_t vblank_us, uint32_t from, uint32_t to) {
        const int k = nshows.load(std::memory_order_relaxed);
        for (int i = 0; i < k; i++)
            if (shows[i].from <= from && shows[i].to >= to) return;   // up already
        if (k >= kShows) return;
        shows[k] = { from, to, vblank_us };
        nshows.store(k + 1, std::memory_order_release);
    }
};
using SliceTimesPtr = std::shared_ptr<SliceTimes>;

// One slice's way to the screen, in us: the stages the HUD shows.
struct SliceLatency {
    uint32_t enc = 0;      // first row captured -> out of the encoder
    uint32_t rf = 0;       // out of the encoder -> here
    uint32_t dec = 0;      // here -> its rows decoded
    uint32_t disp = 0;     // decoded -> the scan-out lights its first row
    uint32_t total() const { return enc + rf + dec + disp; }
};

// Slice i, through the flip that first put its rows up (lit_vblank). row_us(r)
// is when the scan-out reaches screen row r after a vblank, screen_rows how
// many there are, period_us a refresh. A slice decoded after the scan-out
// passed its rows shows at the next refresh. False if it is not complete.
template <typename RowTime>
bool slice_latency(const SliceTimes& st, int i, double period_us,
                   int screen_rows, RowTime row_us, SliceLatency& out) {
    const SliceTimes::Slice& s = st.s[i];
    const uint64_t done = s.done_us.load(), vblank_us = st.lit_vblank(s.row);
    if (!s.cap_us || !s.out_us || !s.here_us || !done || !vblank_us || !st.height) return false;
    if (s.out_us < s.cap_us || s.here_us < s.out_us || done < s.here_us) return false;
    // As if the picture filled the screen: in a gallery tile, the stats
    // screen's live picture or a smaller Picture Size this is the live
    // view's figure, not the tile's own scan-out.
    const int srow = (int)((uint64_t)s.row * (uint64_t)screen_rows / st.height);
    double lit = (double)vblank_us + row_us(srow);
    for (int k = 0; k < 4 && period_us > 0 && (double)done > lit; k++) lit += period_us;
    if ((double)done > lit) return false;
    out.enc = (uint32_t)(s.out_us - s.cap_us);
    out.rf = (uint32_t)(s.here_us - s.out_us);
    out.dec = (uint32_t)(done - s.here_us);
    out.disp = (uint32_t)(lit - (double)done);
    return true;
}

// The slowest of a picture's slices (see slice_latency); *which is its index.
template <typename RowTime>
bool slowest_slice(const SliceTimes& st, double period_us,
                   int screen_rows, RowTime row_us, SliceLatency& out, int* which = nullptr) {
    const int n = st.n.load() < SliceTimes::kMax ? st.n.load() : SliceTimes::kMax;
    bool any = false;
    for (int i = 0; i < n; i++) {
        SliceLatency l;
        if (!slice_latency(st, i, period_us, screen_rows, row_us, l)) continue;
        if (!any || l.total() > out.total()) {
            out = l;
            if (which) *which = i;
        }
        any = true;
    }
    return any;
}

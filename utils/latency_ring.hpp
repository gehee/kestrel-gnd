#ifndef LATENCY_RING_HPP
#define LATENCY_RING_HPP

// The last three minutes of per-frame latency, for the canopy's median and
// the debug graphs.
//
// This was a std::vector that every video frame appended to and, once full,
// erased from the front - moving 21600 entries (~780 KB) per frame - and that
// every HUD frame copied whole under osd_mutex, then sorted a second copy of
// to find the median. All of that just to print one number. Now it is a
// fixed ring, and the median comes from a histogram kept up to date as
// frames enter and leave, so a push is O(1) and the median a walk over the
// bins. The canopy prints it to the whole millisecond; the bins are 0.5 ms up
// to 200 ms, then 5 ms up to 6 s for a link that is badly off.

#include <cstddef>
#include <cstdint>
#include <vector>
// LatencyFrame comes from osd.hpp, which includes this after defining it.

class LatencyRing {
    public:
        static constexpr size_t kCapacity = 21600;   // 3 minutes at 120 fps

        void push(const LatencyFrame& f) {
            if (count_ == kCapacity) {
                unbin(buf_[head_]);
                buf_[head_] = f;
                head_ = (head_ + 1) % kCapacity;
            } else {
                buf_[(head_ + count_) % kCapacity] = f;
                count_++;
            }
            bin(f);
        }

        size_t size() const { return count_; }
        bool   empty() const { return count_ == 0; }
        // i = 0 is the oldest.
        const LatencyFrame& at(size_t i) const { return buf_[(head_ + i) % kCapacity]; }
        const LatencyFrame& back() const { return at(count_ - 1); }

        // Oldest-first copy of the newest n entries (all of them if n is 0).
        void copy_to(std::vector<LatencyFrame>& out, size_t n = 0) const {
            if (n == 0 || n > count_) n = count_;
            out.resize(n);
            for (size_t i = 0; i < n; i++) out[i] = at(count_ - n + i);
        }

        // Median total latency of the frames that have one, in ms; false if
        // none do. Resolution is the bin width.
        bool median(float& ms) const {
            if (binned_ == 0) return false;
            size_t target = binned_ / 2, seen = 0;
            for (int b = 0; b < kBins; b++) {
                seen += hist_[b];
                if (seen > target) { ms = bin_centre(b); return true; }
            }
            ms = bin_centre(kBins - 1);
            return true;
        }

    private:
        static constexpr float kFineMs    = 0.5f;           // 0..200 ms
        static constexpr int   kFine      = 400;
        static constexpr float kCoarseMs  = 5.0f;           // 200 ms..6 s
        static constexpr int   kCoarse    = 1160;
        static constexpr int   kBins      = kFine + kCoarse; // beyond 6 s lands in the last
        static float bin_centre(int b) {
            return b < kFine ? (b + 0.5f) * kFineMs
                             : kFine * kFineMs + (b - kFine + 0.5f) * kCoarseMs;
        }

        static float total(const LatencyFrame& f) {
            return f.capture_ms + f.processing_ms + f.net_ms +
                   f.reassemble_ms + f.dec_ms + f.disp_ms;
        }
        // Gap entries (all zero) carry no latency and stay out of the median,
        // as they did when it was computed by sorting.
        static int bin_of(const LatencyFrame& f) {
            float t = total(f);
            if (!(t > 0.1f)) return -1;
            if (t < kFine * kFineMs) return (int)(t / kFineMs);
            int b = kFine + (int)((t - kFine * kFineMs) / kCoarseMs);
            return b >= kBins ? kBins - 1 : b;
        }
        void bin(const LatencyFrame& f)   { int b = bin_of(f); if (b >= 0) { hist_[b]++; binned_++; } }
        void unbin(const LatencyFrame& f) { int b = bin_of(f); if (b >= 0) { hist_[b]--; binned_--; } }

        std::vector<LatencyFrame> buf_ = std::vector<LatencyFrame>(kCapacity);
        size_t   head_ = 0, count_ = 0;
        uint32_t hist_[kBins] = {};
        size_t   binned_ = 0;
};

#endif

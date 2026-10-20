#pragma once
// The radio clock (AP time) to some 10-20 us, from the radio's TDD frame.
//
// The radio clock reads in whole ms here, and this radio's copy of it is set
// from the air unit's about once a minute, with no allowance for the link: our
// offset to it (polled once a second) wanders by a ms or so and now and then
// jumps. The frame, though, is the air radio's, which runs it off the same
// 40 MHz as its clock, in whole 0.64 us ticks; ours follows it. kestrel-air
// finds its frame's edges (where the video part ends) on its own clock, counts
// them on the radio clock and sends us where they fall on it (KA_MSG_FRAME:
// the frame Tt, the phase phi); here the same edges are found on our radio
// (utils/frame_edge.h) and counted: which edge is which by our coarse offset
// once, then by counting them - through losing and finding the frame again
// too, while it keeps its period - as the coarse offset drifts by several us
// a second for minutes (seen 2026-10-11, up to ~4 ms off) and can be a frame
// or more off already. The owner corrects that from the slices, right after
// each count (shift(): the fastest of a few hundred, free-running, gets here
// 2.4 ms to 2.4 ms and a frame after leaving the encoder - 3.6-4.1 ms seen at
// 40 MHz, 5.1-5.9 at 20). An air unit that found only every other edge (one
// before 2026-10-14 on a 3.25 ms frame: 6501.12) is counted on our edges,
// since each of its edges is one of ours. Then a time of ours is phi + m Tt plus what has passed
// since edge m, on both ends alike - give or take how much later this radio's
// video part ends than the air's (a constant). Like kestrel-air's, it is the
// frame's clock: it drifts from the radio clock's ms by some 10 us a second.
#include "frame_edge.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

class FrameClock {
public:
    // Reads the TDD state byte (0x0a110e6e); 0: read.
    using ReadFn = std::function<int(uint8_t *)>;
    // Our coarse offset's radio clock at our time t (us); false: none.
    using CoarseFn = std::function<bool(uint64_t, double *)>;

    ~FrameClock() { stop(); }

    void start(ReadFn rd, CoarseFn coarse) {
        stop();
        rd_ = std::move(rd);
        coarse_ = std::move(coarse);
        run_ = true;
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        run_ = false;
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> lk(mu_);
        valid_ = false;
    }

    // KA_MSG_FRAME from kestrel-air.
    void set_air_frame(double Tt, double phi, unsigned gen, int frames_per_picture) {
        std::lock_guard<std::mutex> lk(mu_);
        air_nf_ = frames_per_picture;
        if (!air_known_ || gen != air_gen_ || std::fabs(Tt - air_Tt_) > 0.01 || std::fabs(phi - air_phi_) > 0.5)
            printf("frameclock: air unit's frame %.2f us, video ends at %.1f on the radio clock (generation %u)\n",
                   Tt, phi, gen);
        // Another frame, or this one restarted elsewhere (the air's phase
        // moved more than its own count's noise): counted again.
        if (air_known_ && (std::fabs(Tt - air_Tt_) > 0.01 || std::fabs(fe_wrap(phi - air_phi_, Tt)) > 150))
            recount_ = true;
        air_Tt_ = Tt; air_phi_ = phi; air_gen_ = gen;
        air_known_ = true;
    }
    // The frame the radio clock is counted on, us (0: not counted).
    double frame_period() {
        std::lock_guard<std::mutex> lk(mu_);
        return valid_ ? Tt_ : 0;
    }
    // Our own radio's frame on our clock while it is followed, us (0: not
    // found) - counted or not, with any air unit.
    double local_period() {
        std::lock_guard<std::mutex> lk(mu_);
        return following_ ? Tm_ : 0;
    }
    unsigned air_gen() {
        std::lock_guard<std::mutex> lk(mu_);
        return air_gen_;
    }
    // Radio frames a picture would take under the frame lock (0: not said).
    int air_frames_per_picture() {
        std::lock_guard<std::mutex> lk(mu_);
        return air_nf_;
    }
    // Our count of edges is k frames short (negative: ahead); applied by the loop.
    void shift(int64_t k) { shift_.fetch_add(k); }
    // Changes whenever the edges are counted anew (from the coarse offset).
    unsigned count_gen() const { return count_gen_.load(); }

    // The radio clock at our time t, us. false: not known.
    bool ap_us(uint64_t t, uint64_t *ap) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!valid_ || std::fabs((double)t - E_) > 3e6) return false;
        const double v = air_phi_ + (double)m_ * Tt_ + ((double)t - E_) * (Tt_ / Tm_);
        if (v < 0) return false;
        *ap = (uint64_t)std::llround(v);
        return true;
    }

private:
    static uint64_t mono_us() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
    }
    static void sleep_until(uint64_t t) {
        struct timespec ts = {(time_t)(t / 1000000), (long)(t % 1000000) * 1000};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) != 0) {}
    }
    bool read(fe_read *r) {
        uint8_t b = 0;
        const uint64_t t0 = mono_us();
        if (rd_(&b) != 0) return false;
        r->t0 = t0;
        r->rtt = (uint32_t)(mono_us() - t0);
        r->video = (uint8_t)fe_is_video(b);
        return true;
    }

    void loop() {
        std::vector<fe_read> buf;
        std::mt19937 rng((unsigned)mono_us());
        uint64_t last_fail_log = 0;
        // The count carried from one finding of the frame to the next: an edge
        // of the last, its index, the period.
        bool carried = false;
        double E_last = 0, Tm_last = 0;
        int64_t m_last = 0;
        while (run_) {
            { std::lock_guard<std::mutex> lk(mu_); valid_ = false; following_ = false; }
            // Tests: /tmp/kestrel-frameclock-off - no reads at all.
            if (access("/tmp/kestrel-frameclock-off", F_OK) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // Find the frame: reads as fast as the radio answers, 1.5 s.
            buf.clear();
            const uint64_t acq_end = mono_us() + 1500000;
            while (run_ && mono_us() < acq_end) {
                fe_read r;
                if (read(&r)) buf.push_back(r);
                else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            double Tm = 0, E = 0, pur = 0;
            const int rc = buf.size() < 300 ? -1 : fe_acquire(buf.data(), (int)buf.size(), &Tm, &E, &pur);
            if (rc) {
                if (mono_us() - last_fail_log > 30000000) {
                    printf("frameclock: no frame found (%d, %zu reads, %.3f sorted)\n", rc, buf.size(), pur);
                    last_fail_log = mono_us();
                }
                for (int i = 0; i < 20 && run_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            printf("frameclock: frame %.3f us of ours, %zu reads %.3f sorted\n", Tm, buf.size(), pur);
            // Follow it: one read a frame aimed around the next edge, the loop
            // every 0.5 s; edges counted with the coarse clock.
            int64_t m = 0;
            int good = 0, misses = 0;
            bool counted = false;
            if (carried && std::fabs(Tm - Tm_last) < 0.5) {
                const double k = (E - E_last) / Tm_last;
                if (std::fabs(k - std::round(k)) < 0.05) {          // the same edges as before
                    m = m_last + std::llround(k);
                    counted = true;
                    printf("frameclock: edges counted on from before (%.1f s)\n", (E - E_last) / 1e6);
                }
            }
            std::vector<int64_t> cand;      // which edge ours is by the coarse clock, of late
            uint64_t last_up = mono_us(), last_log = 0;
            double s = 0, w = 0;
            buf.clear();
            std::uniform_int_distribution<int> aim(-800, 199);
            while (run_) {
                // Once followed well, one frame in six (~30 reads a second):
                // every read shares the USB link with the video. (The stats'
                // Dec wait of 0.8 ms seen then was not these reads but the
                // video loop's 1 ms waits on its other sockets, 2026-10-12.)
                const bool settled = good >= 20;
                const uint64_t now = mono_us();
                const double En = E + (std::ceil(((double)now + 1000 - E) / Tm) + (settled ? 5 : 0)) * Tm;
                sleep_until((uint64_t)(En + aim(rng)));
                fe_read r;
                if (read(&r)) buf.push_back(r);
                const uint64_t t = mono_us();
                if (t - last_up < (settled ? 1000000u : 500000u)) continue;
                if (access("/tmp/kestrel-frameclock-off", F_OK) == 0) break;
                const double N = ((double)t - (double)last_up) / Tm;
                last_up = t;
                if (!buf.empty()) {
                    const double j = std::floor(((double)buf[0].t0 - E) / Tm);
                    if (j > 0) { E += j * Tm; m += (int64_t)j; }
                }
                if (!buf.empty() && fe_measure(buf.data(), (int)buf.size(), E, Tm, &s, &w) == 0 && std::fabs(s) < 400) {
                    E += 0.3 * s;
                    Tm += 0.05 * s / N;
                    good++;
                    misses = 0;
                } else {
                    misses++;
                }
                buf.clear();
                if (misses >= 6) {
                    printf("frameclock: lost the frame, finding it again\n");
                    break;
                }
                const double Tt = std::round(Tm / FE_TICK_US) * FE_TICK_US;
                // Which edge is which: the air's phase, our coarse clock at the edge.
                double ac = 0;
                bool air;
                double aTt, aphi;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    air = air_known_;
                    aTt = air_Tt_;
                    aphi = air_phi_;
                    // The air unit's frame two or three of ours: counted on ours.
                    const double k = std::round(aTt / Tt);
                    if (k >= 2 && k <= 3 && std::fabs(aTt - k * Tt) < 0.5 * k) aTt /= k;
                    if (recount_) { recount_ = false; counted = false; carried = false; cand.clear(); }
                    const int64_t sh = shift_.exchange(0);
                    if (sh && counted) {
                        m += sh;
                        printf("frameclock: our count of edges moved %+lld frames (by the slices' latency)\n", (long long)sh);
                    }
                }
                if (air && std::fabs(aTt - Tt) < 0.5 && good >= 6 && coarse_((uint64_t)E, &ac)) {
                    cand.push_back(std::llround((ac - aphi) / aTt) - m);
                    if (cand.size() > 21) cand.erase(cand.begin());
                    if (cand.size() >= 7) {
                        std::vector<int64_t> c = cand;
                        std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end());
                        const int64_t k = c[c.size() / 2];
                        if (!counted) {
                            m += k;
                            for (auto &x : cand) x -= k;
                            counted = true;
                            count_gen_++;
                            printf("frameclock: radio clock on the frame's edges (our frame %.3f us, %.2f radio)\n", Tm, Tt);
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    valid_ = counted && air && misses < 3 && std::fabs(aTt - Tt) < 0.5;
                    following_ = misses < 3;
                    E_ = E; Tm_ = Tm; Tt_ = aTt; m_ = m;
                }
                if (counted && misses == 0) { carried = true; E_last = E; Tm_last = Tm; m_last = m; }
                if (t - last_log >= 30000000) {
                    double off = 0;
                    const bool have = counted && coarse_(t, &off);
                    uint64_t fine = 0;
                    if (have && ap_us(t, &fine)) off -= (double)fine; else off = 0;
                    printf("frameclock: frame %.3f us of ours, edge %+.1f us (bounds %.0f apart)%s, coarse clock %+.0f us off\n",
                           Tm, s, w, counted ? "" : (air ? ", not counted yet" : ", no frame from the air unit"), off);
                    last_log = t;
                }
            }
        }
    }

    ReadFn rd_;
    CoarseFn coarse_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<int64_t> shift_{0};
    std::atomic<unsigned> count_gen_{0};
    std::mutex mu_;
    bool valid_ = false, air_known_ = false, recount_ = false, following_ = false;
    double E_ = 0, Tm_ = 1, Tt_ = 0, air_Tt_ = 0, air_phi_ = 0;
    int64_t m_ = 0;
    unsigned air_gen_ = 0;
    int air_nf_ = 0;
};

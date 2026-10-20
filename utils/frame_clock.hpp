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
//
// A new frame (another width or frame mode, the link formed again) is found
// again from a quarter of a second of reads against the frames it can be
// (frame_edge.h's table) on our clock by the count's rate, and counted on from
// the last count: the radio clock runs on through the change, the air unit
// carries its own onto the new frame alike and says where that frame's video
// part ends on it - which edge is which follows from our clock at the edge.
// The last count is used on meanwhile. Then the slices check whole frames, as
// after any count (an air unit that counted from its ms readings again).
//
// With Open8030 at both ends there is nothing to count: the radios' frame
// stamps give our radio the air radio's clock at its latest frame interrupt
// (IRQ 5; request 0x0c5a0035). Each step reads it: when that IRQ 5 was on our
// clock (to the request's round trip) only says which of our edges it is;
// the edge, followed to some 10 us, gets the air radio's clock - kestrel-air
// does the same from its own radio. One clock at both ends, to the frame,
// from the first step and through any change of the frame (exact()). Before
// the edge is found (each search, after a link forms or a frame changes) the
// stamps alone set it, to the request's round trip (stamps_only()).
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

// Open8030's frame info (its ar_rf_frame_info).
struct FrameStampInfo {
    uint32_t version, flags, frames, frame_ticks, since_us, ap_us_lo, ap_us_hi;
    uint32_t taken, rejected;
    int32_t residual_us;
    uint32_t width, frame_mode, sent;
    uint32_t retries, steady, retry_steps;   // blocks sent again, steady mode, its steps down
};

class FrameClock {
public:
    // Our radio's frame info (Open8030; 0: read). Not set or failing: the
    // frame is counted as before.
    using InfoFn = std::function<int(FrameStampInfo *)>;
    void set_info(InfoFn fn) { info_ = std::move(fn); }
    // The clock comes from the radios' frame stamps (nothing counted).
    bool exact() const { return exact_.load(); }
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
    // The radio's frame may have changed (a width event, a frame mode set):
    // found again at once.
    void reframe() { reframe_ = true; }

    // KA_MSG_FRAME from kestrel-air.
    void set_air_frame(double Tt, double phi, unsigned gen, int frames_per_picture) {
        std::lock_guard<std::mutex> lk(mu_);
        air_nf_ = frames_per_picture;
        // Logged when it is another frame or count (an air unit on its radio's
        // frame interrupts moves its phase a few us every message: the frame's
        // timing runs ~8 ppm off the radio's timer).
        if (!air_known_ || gen != air_gen_ || std::fabs(Tt - air_Tt_) > 0.01 || std::fabs(fe_wrap(phi - air_phi_, Tt)) > 150)
            printf("frameclock: air unit's frame %.2f us, video ends at %.1f on the radio clock (generation %u)\n",
                   Tt, phi, gen);
        // Another frame, or this one restarted elsewhere (the air's phase
        // moved more than its own count's noise): counted again - on from
        // ours where it is fresh. Against the last one said, before a link-up
        // too.
        if (air_said_ && (std::fabs(Tt - air_Tt_) > 0.01 || std::fabs(fe_wrap(phi - air_phi_, Tt)) > 150))
            recount_ = true;
        air_Tt_ = Tt; air_phi_ = phi; air_gen_ = gen;
        air_known_ = air_said_ = true;
    }
    // A link-up: the air unit may be another one, or the same with stock's
    // app, which sends no frame - kept, it held a stock air unit's figures
    // back for good, waiting on a check only kestrel-air's stamps pass
    // (2026-10-20: a Lite flashed to stock, RF never shown). kestrel-air says
    // its frame again within a second, inside the link-up's resync.
    void forget_air() {
        std::lock_guard<std::mutex> lk(mu_);
        air_known_ = false;
    }
    // The frame the radio clock is counted on, us (0: not counted).
    double frame_period() {
        std::lock_guard<std::mutex> lk(mu_);
        return valid_ && (double)mono_us() < until_ ? cTt_ : 0;
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
    // The air unit sends its frame (kestrel-air with a frame clock).
    bool air_known() {
        std::lock_guard<std::mutex> lk(mu_);
        return air_known_;
    }
    // Radio frames a picture would take under the frame lock (0: not said).
    int air_frames_per_picture() {
        std::lock_guard<std::mutex> lk(mu_);
        return air_nf_;
    }
    // Our count of edges is k frames short (negative: ahead); applied by the loop.
    void shift(int64_t k) { shift_.fetch_add(k); }
    // Shifts taken by the loop (applied, or dropped while not counted).
    unsigned shifts_taken() const { return shifts_taken_.load(); }
    // Changes whenever the edges are counted anew (from the coarse offset).
    unsigned count_gen() const { return count_gen_.load(); }

    // The radio clock at our time t, us. false: not known.
    bool ap_us(uint64_t t, uint64_t *ap) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!valid_ || (double)t >= until_ || (double)t + 3e6 < cE_) return false;
        const double v = cphi_ + (double)cm_ * cTt_ + ((double)t - cE_) * (cTt_ / cTm_);
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

    // A count: the radio clock at our time t is phi + m Tt + (t - E) Tt/Tm.
    struct Count {
        double E = 0, Tm = 1, Tt = 0, phi = 0;
        int64_t m = 0;
        unsigned gen = 0;               // the air unit's frame it was counted on
        double at(double t) const { return phi + (double)m * Tt + (t - E) * (Tt / Tm); }
    };
    void publish(const Count &c, double until) {
        std::lock_guard<std::mutex> lk(mu_);
        cE_ = c.E; cTm_ = c.Tm; cTt_ = c.Tt; cphi_ = c.phi; cm_ = c.m;
        until_ = until;
        valid_ = true;
    }

    // A stamp is fresh while the radio takes new ones (every 100 ms on a
    // link): with the link gone our radio still says the last link's.
    bool stamp_fresh(const FrameStampInfo &si, uint64_t now) {
        if (si.taken != last_taken_) { last_taken_ = si.taken; last_taken_us_ = now; return true; }
        return last_taken_us_ && now - last_taken_us_ < 400000;
    }
    // The air radio's clock (32 bits of us, 71.6 min) unwrapped; taken anew
    // when it is not where it should be by now - the air unit started again.
    void take_air_clock(uint32_t s32, uint64_t now) {
        if (s_known_) {
            const double expect = (double)s64_ + ((double)now - (double)s_us_);
            const int64_t next = (int64_t)s64_ + (int64_t)(int32_t)(s32 - (uint32_t)s64_);
            if (std::fabs((double)next - expect) < 2e6) { s64_ = (uint64_t)next; s_us_ = now; return; }
            printf("frameclock: the air radio's clock is not where it was (%.1f s off): it started again\n",
                   ((double)next - expect) / 1e6);
        }
        s64_ = s32;
        s_us_ = now;
        s_known_ = true;
    }

    // Open8030 at both ends, while our radio's frame edge is not found (the
    // link just formed, its frame mode set, the edge search under way): the
    // radio clock from the stamps alone. A stamp says how long ago its IRQ 5
    // was, so the request's round trip places it - the fastest of a few, to
    // some 0.2 ms - with no edge to pin it to; the frame is the radio's own.
    // The latency figures then come ~3 s into a link (the stamps' start), not
    // once the width has settled and an edge search has run (12-15 s); the
    // edge, once followed, takes over (to some 10 us). False: no stamps.
    bool stamps_only(bool carried, const Count &last) {
        if (!info_) return false;
        FrameStampInfo best{};
        uint64_t best_rtt = UINT64_MAX, best_mid = 0;
        for (int i = 0; i < 5 && run_; i++) {
            FrameStampInfo si{};
            const uint64_t r0 = mono_us();
            const int rc = info_(&si);
            const uint64_t r1 = mono_us();
            if (rc != 0) break;                 // not answered: not Open8030, or not yet
            if ((si.flags & 2) && si.frame_ticks && r1 - r0 < best_rtt) {
                best = si; best_rtt = r1 - r0; best_mid = (r0 + r1) / 2;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        if (best_rtt > 3000 || !stamp_fresh(best, mono_us())) {   // none, too slow, or stale
            exact_ = false;
            stamp_only_ = false;
            return false;
        }
        const double sTt = best.frame_ticks * FE_TICK_US;
        take_air_clock(best.ap_us_lo, best_mid);
        Count c;
        // The IRQ 5 on our clock, less its usual lead on our edge: the time the
        // edge-pinned clock gives the same stamp, so the hand-over is seamless.
        c.E = (double)best_mid - (double)best.since_us - (g_n_ ? g_avg_ : 0);
        c.Tt = sTt;
        // Our clock against the radio's: as last measured, else as one.
        c.Tm = carried && last.Tt > 0 ? sTt * (last.Tm / last.Tt) : sTt;
        c.m = (int64_t)std::floor((double)s64_ / sTt);
        c.phi = (double)s64_ - (double)c.m * sTt;
        {
            std::lock_guard<std::mutex> lk(mu_);
            c.gen = air_gen_;
        }
        publish(c, (double)mono_us() + 3e6);
        if (!stamp_only_) {
            printf("frameclock: the radio clock from the frame stamps alone, before our frame's edge is found "
                   "(frame %.2f us, round trip %llu us)\n", sTt, (unsigned long long)best_rtt);
            count_gen_++;
            stamp_only_ = true;
        }
        exact_ = true;
        return true;
    }

    void loop() {
        std::vector<fe_read> buf;
        std::mt19937 rng((unsigned)mono_us());
        uint64_t last_fail_log = 0;
        // The count carried from one finding of the frame to the next: at its
        // last good step.
        bool carried = false;
        Count last;
        // Finding the frame again after a change: the table first.
        bool quick = false;
        while (run_) {
            { std::lock_guard<std::mutex> lk(mu_); following_ = false; }
            double Tm = 0, E = 0, pur = 0;
            int rc = -1;
            reframe_ = false;               // what is found now is after it
            stamps_only(carried, last);     // meanwhile, from the stamps (Open8030)
            // After a change: which of the frames it can be, on our clock by
            // the last count's rate (ours against the radio's holds through it).
            // Twice: a frame mode set is told at once but changes some frames
            // later, and reads across the change fit neither frame.
            for (int pass = 0; pass < 2 && rc && quick && carried; pass++) {
                buf.clear();
                const uint64_t end = mono_us() + FE_PHASE_US;
                while (run_ && mono_us() < end) {
                    fe_read r;
                    if (read(&r)) buf.push_back(r);
                    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                const double rate = last.Tm / last.Tt;
                for (int i = -1; i < 6; i++) {
                    const double T = (i < 0 ? last.Tt : fe_frames[i]) * rate;
                    double e, p;
                    if (fe_phase(buf.data(), (int)buf.size(), T, &e, &p) == 0 && p > pur) { pur = p; E = e; Tm = T; rc = 0; }
                }
                if (!rc) printf("frameclock: frame %.3f us of ours (%.2f radio), %zu reads %.3f sorted\n", Tm,
                                std::round(Tm / FE_TICK_US) * FE_TICK_US, buf.size(), pur);
                else if (pass) printf("frameclock: not one of the known frames (%zu reads), searching\n", buf.size());
            }
            quick = false;
            if (rc) {
                // Find the frame: reads as fast as the radio answers, 1.5 s.
                buf.clear();
                const uint64_t acq_end = mono_us() + 1500000;
                while (run_ && mono_us() < acq_end) {
                    fe_read r;
                    if (read(&r)) buf.push_back(r);
                    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                rc = buf.size() < 300 ? -1 : fe_acquire(buf.data(), (int)buf.size(), &Tm, &E, &pur);
                if (rc) {
                    if (mono_us() - last_fail_log > 30000000) {
                        printf("frameclock: no frame found (%d, %zu reads, %.3f sorted)\n", rc, buf.size(), pur);
                        last_fail_log = mono_us();
                    }
                    for (int i = 0; i < 20 && run_; i++) {
                        if (i % 5 == 0) stamps_only(carried, last);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    continue;
                }
                printf("frameclock: frame %.3f us of ours, %zu reads %.3f sorted\n", Tm, buf.size(), pur);
            }
            // Follow it: one read a frame aimed around the next edge, the loop
            // every 0.5 s; edges counted with the coarse clock.
            int64_t m = 0;
            int good = 0, misses = 0;
            bool counted = false;
            Count cur;                      // what the count is on (the air's frame, its phase)
            if (carried && std::fabs(Tm - last.Tm) < 0.5) {
                const double k = (E - last.E) / last.Tm;
                if (std::fabs(k - std::round(k)) < 0.05) {          // the same edges as before
                    m = last.m + std::llround(k);
                    cur = last;
                    counted = true;
                    printf("frameclock: edges counted on from before (%.1f s)\n", (E - last.E) / 1e6);
                }
            }
            std::vector<int64_t> cand;      // which edge ours is by the coarse clock, of late
            bool restart_logged = false;
            uint64_t last_up = mono_us(), last_log = 0;
            double s = 0, w = 0;
            buf.clear();
            std::uniform_int_distribution<int> aim(-800, 199);
            while (run_) {
                if (reframe_.exchange(false)) {
                    printf("frameclock: the radio's frame may have changed, finding it again\n");
                    quick = true;
                    break;
                }
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
                if (misses >= 2) {
                    printf("frameclock: lost the frame, finding it again\n");
                    quick = true;
                    break;
                }
                const double Tt = std::round(Tm / FE_TICK_US) * FE_TICK_US;
                // Which edge is which: the air's phase, our coarse clock at the edge.
                double ac = 0;
                bool air;
                double aTt, aphi;
                unsigned agen;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    air = air_known_;
                    aTt = air_Tt_;
                    aphi = air_phi_;
                    agen = air_gen_;
                    // The air unit's frame two or three of ours: counted on ours.
                    const double k = std::round(aTt / Tt);
                    if (k >= 2 && k <= 3 && std::fabs(aTt - k * Tt) < 0.5 * k) aTt /= k;
                    // (On the frame stamps the air's phase plays no part.)
                    if (recount_) { recount_ = false; if (!exact_) { counted = false; cand.clear(); } }
                    const int64_t sh = shift_.exchange(0);
                    if (sh && counted) {
                        m += sh;
                        printf("frameclock: our count of edges moved %+lld frames (by the slices' latency)\n", (long long)sh);
                    }
                    if (sh) shifts_taken_++;
                }
                // Open8030 at both ends: the air radio's clock at our radio's
                // latest IRQ 5, on the edge nearest it.
                bool stamped = false;
                if (info_) {
                    FrameStampInfo si{};
                    const uint64_t r0 = mono_us();
                    const int rc = info_(&si);
                    const uint64_t r1 = mono_us();
                    if (rc == 0 && (si.flags & 2) && si.frame_ticks && r1 - r0 < 3000 && stamp_fresh(si, r1)) {
                        const double irq5 = (double)(r0 + r1) / 2 - (double)si.since_us;
                        const double k = std::round((irq5 - E) / Tm);
                        const double g = irq5 - (E + k * Tm);
                        const double sTt = si.frame_ticks * FE_TICK_US;
                        if (std::fabs(sTt - Tt) < 0.5 && std::fabs(g) < Tm / 2 - 300) {
                            // The air radio's clock: 32 bits of us (71.6 min), unwrapped.
                            take_air_clock(si.ap_us_lo, (r0 + r1) / 2);
                            const double vE = (double)s64_ - k * sTt;   // at our edge E
                            m = (int64_t)std::floor(vE / sTt);
                            cur.Tt = sTt; cur.phi = vE - (double)m * sTt; cur.gen = agen;
                            if (!counted || !exact_ || stamp_only_) {
                                printf("frameclock: the radio clock from the frame stamps (frame %.2f us, IRQ 5 %+.0f us from our edge)\n",
                                       sTt, g);
                                count_gen_++;
                            }
                            stamp_only_ = false;
                            counted = true;
                            stamped = true;
                            cand.clear();
                            g_avg_ = g_n_ ? g_avg_ + (g - g_avg_) / 16 : g;
                            g_n_++;
                        }
                    }
                }
                exact_ = stamped;
                // A new frame, or the air's count on it: on from the last count
                // at this edge's time, its phase the air's - once the air unit
                // has said where it ends (a frame of its newer than the count's).
                // Not when our radio's copy of the radio clock disagrees by a
                // second or more: the air unit started again (its radio's clock
                // with it) - carried, the count was 253 s off, out of the
                // slices' check's reach (bench, a stock air radio rebooted).
                double ac_now = 0;
                const bool restarted = carried && coarse_((uint64_t)E, &ac_now) && std::fabs(last.at(E) - ac_now) > 1e6;
                if (restarted && !counted && good >= 3 && !restart_logged) {
                    printf("frameclock: the radio clock is %.1f s from the last count: the air unit started again, counted anew\n",
                           (ac_now - last.at(E)) / 1e6);
                    restart_logged = true;
                }
                // A count carried from before this link (the edges lined up
                // with the old grid) is the old air unit's: with the radio
                // clock a second or more off it, dropped, and nothing more
                // carried - else the stale clock was published and every
                // slice failed the check, with no air figures until the next
                // reframe. The stamps' count is the air radio's own: kept.
                if (restarted && counted && !stamped) {
                    printf("frameclock: the radio clock is %.1f s from the count carried on: the air unit started again, dropped\n",
                           (ac_now - last.at(E)) / 1e6);
                    counted = false;
                    cand.clear();
                    carried = false;
                    restart_logged = true;
                }
                if (!stamped && !counted && carried && !restarted && air && std::fabs(aTt - Tt) < 0.5 && good >= 3 && E - last.E < 60e6 &&
                    (agen != last.gen || std::fabs(aTt - last.Tt) > 0.5)) {
                    m = std::llround((last.at(E) - aphi) / aTt);
                    cur.Tt = aTt; cur.phi = aphi; cur.gen = agen;
                    counted = true;
                    cand.clear();
                    count_gen_++;
                    printf("frameclock: counted on from before onto a new frame (%.2f -> %.2f us)\n", last.Tt, aTt);
                }
                if (!stamped && air && std::fabs(aTt - Tt) < 0.5 && good >= 6 && coarse_((uint64_t)E, &ac)) {
                    cand.push_back(std::llround((ac - aphi) / aTt) - m);
                    if (cand.size() > 21) cand.erase(cand.begin());
                    if (cand.size() >= 7) {
                        std::vector<int64_t> c = cand;
                        std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end());
                        const int64_t k = c[c.size() / 2];
                        if (!counted) {
                            m += k;
                            for (auto &x : cand) x -= k;
                            cur.Tt = aTt; cur.phi = aphi; cur.gen = agen;
                            counted = true;
                            count_gen_++;
                            printf("frameclock: radio clock on the frame's edges (our frame %.3f us, %.2f radio)\n", Tm, Tt);
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    following_ = misses < 3;
                    Tm_ = Tm;
                }
                if (counted && misses == 0 && std::fabs(cur.Tt - Tt) < 0.5) {
                    cur.E = E; cur.Tm = Tm; cur.m = m;
                    publish(cur, (double)t + 3e6);
                    last = cur;
                    carried = true;
                }
                if (t - last_log >= 30000000) {
                    double off = 0;
                    const bool have = counted && coarse_(t, &off);
                    uint64_t fine = 0;
                    if (have && ap_us(t, &fine)) off -= (double)fine; else off = 0;
                    printf("frameclock: frame %.3f us of ours, edge %+.1f us (bounds %.0f apart)%s, coarse clock %+.0f us off%s\n",
                           Tm, s, w, counted ? "" : (air ? ", not counted yet" : ", no frame from the air unit"), off,
                           exact_ ? " (frame stamps)" : "");
                    if (exact_) printf("frameclock: frame stamps: IRQ 5 %+.1f us from our edge on average\n", g_avg_);
                    last_log = t;
                }
            }
            // The last count goes on while the frame is found again.
            if (quick && carried) {
                std::lock_guard<std::mutex> lk(mu_);
                if (valid_) until_ = std::max(until_, last.E + 7e6);
            }
        }
    }

    ReadFn rd_;
    CoarseFn coarse_;
    std::thread th_;
    InfoFn info_;
    std::atomic<bool> exact_{false};
    bool stamp_only_ = false;            // the loop thread: the clock is from stamps alone
    uint32_t last_taken_ = 0;            // the loop thread: stamps taken, as last read
    uint64_t last_taken_us_ = 0;         // ... and when that count last moved
    uint64_t s_us_ = 0;                  // our time at s64_
    std::atomic<bool> run_{false}, reframe_{false};
    std::atomic<int64_t> shift_{0};
    std::atomic<unsigned> count_gen_{0}, shifts_taken_{0};
    std::mutex mu_;
    bool valid_ = false, air_known_ = false, air_said_ = false, recount_ = false, following_ = false;
    double Tm_ = 1, air_Tt_ = 0, air_phi_ = 0;
    // The clock (publish()): good until until_.
    double cE_ = 0, cTm_ = 1, cTt_ = 0, cphi_ = 0, until_ = 0;
    // The frame stamps' clock unwrapped (us; it wraps at 2^32), and where its IRQ 5 falls from our edge.
    uint64_t s64_ = 0;
    bool s_known_ = false;
    double g_avg_ = 0;
    unsigned g_n_ = 0;
    int64_t cm_ = 0;
    unsigned air_gen_ = 0;
    int air_nf_ = 0;
};

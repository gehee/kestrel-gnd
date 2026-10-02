#include "screen_tap.hpp"

#include <chrono>

#include "renderer.hpp"   // DecodedUnit

void ScreenTap::bury(std::shared_ptr<DecodedUnit>& du) {
    if (du) graveyard_.push_back(std::move(du));
    du.reset();
}

void ScreenTap::set_recording(bool on, const ScreenPair* seed) {
    std::vector<std::shared_ptr<DecodedUnit>> drop;
    {
        std::lock_guard<std::mutex> lk(m_);
        on_ = on;
        // Whatever an earlier recording left goes, either way.
        bury(pending_.video);
        bury(cur_.video);
        has_pending_ = has_cur_ = false;
        sampling_osd_ = 0;
        land_n_ = 0;
        if (on && seed && (seed->video || seed->osd_fb)) {
            cur_ = *seed;
            has_cur_ = true;
        }
        drop.swap(graveyard_);
        graveyard_.reserve(16);
    }
    cv_.notify_all();
}

void ScreenTap::submitted(std::shared_ptr<DecodedUnit> video, uint32_t osd_fb, uint64_t osd_gen,
                          int vx, int vy, int vw, int vh, uint64_t now_us) {
    if (!recording()) return;
    std::lock_guard<std::mutex> lk(m_);
    // Checked again under the lock: a stop may have cleared the tap since.
    if (!on_) return;
    bury(pending_.video);
    pending_.video = std::move(video);
    pending_.osd_fb = osd_fb;
    pending_.osd_gen = osd_gen;
    pending_.vx = vx; pending_.vy = vy; pending_.vw = vw; pending_.vh = vh;
    pending_us_ = now_us;
    has_pending_ = true;
}

void ScreenTap::submit_failed() {
    if (!recording()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (!on_) return;
    bury(pending_.video);
    has_pending_ = false;
}

void ScreenTap::landed(uint32_t seq) {
    if (!recording()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!on_ || !has_pending_) return;
        bury(cur_.video);
        cur_ = std::move(pending_);
        pending_.video.reset();
        cur_.seq = seq;
        has_cur_ = true;
        has_pending_ = false;
        land_seq_[land_n_++ % kParityRing] = seq;
    }
    cv_.notify_all();
}

void ScreenTap::osd_landed(uint32_t osd_fb, uint64_t osd_gen, uint32_t seq) {
    if (!recording()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!on_) return;
        // The video plane keeps whatever it last showed.
        cur_.osd_fb = osd_fb;
        cur_.osd_gen = osd_gen;
        cur_.seq = seq;
        has_cur_ = true;
    }
    cv_.notify_all();
}

void ScreenTap::moved(int vx, int vy, int vw, int vh, uint32_t seq) {
    if (!recording()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!on_ || !has_cur_) return;
        cur_.vx = vx; cur_.vy = vy; cur_.vw = vw; cur_.vh = vh;
        cur_.seq = seq;
    }
    cv_.notify_all();
}

void ScreenTap::drop_pictures() {
    std::vector<std::shared_ptr<DecodedUnit>> drop;
    {
        std::unique_lock<std::mutex> lk(m_);
        // A composite in progress holds its own copy of the picture; it lets
        // go before sample_done(). A blend takes ~5 ms.
        cv_.wait_for(lk, std::chrono::milliseconds(200), [this] { return !sampling_; });
        bury(pending_.video);
        bury(cur_.video);
        has_pending_ = false;
        // Nothing known on screen until the next flip lands.
        has_cur_ = false;
        drop.swap(graveyard_);
        graveyard_.reserve(16);
    }
    // drop goes here, on the caller's thread, before it returns.
}

bool ScreenTap::sample(uint32_t seq, uint64_t vblank_us, ScreenPair& out, int wait_us) {
    std::unique_lock<std::mutex> lk(m_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(wait_us);
    // A flip submitted before this vblank either landed on it or missed it;
    // until its event says which, what the vblank showed is not known.
    while (has_pending_ && pending_us_ < vblank_us &&
           !(has_cur_ && (int32_t)(cur_.seq - seq) >= 0)) {
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
    }
    if (!has_cur_ || (int32_t)(cur_.seq - seq) > 0) return false;
    out = cur_;
    sampling_osd_ = cur_.osd_fb;
    sampling_ = true;
    return true;
}

void ScreenTap::sample_done() {
    {
        std::lock_guard<std::mutex> lk(m_);
        sampling_osd_ = 0;
        sampling_ = false;
    }
    cv_.notify_all();
}

bool ScreenTap::holds_osd(uint32_t fb) {
    if (!fb || !recording()) return false;
    std::lock_guard<std::mutex> lk(m_);
    return fb == sampling_osd_;
}

int ScreenTap::dominant_parity(int div) {
    if (div <= 1) return 0;
    std::lock_guard<std::mutex> lk(m_);
    const int n = land_n_ < kParityRing ? land_n_ : kParityRing;
    if (n == 0) return -1;
    int count[8] = {0};
    if (div > 8) div = 8;
    for (int i = 0; i < n; i++) count[land_seq_[i] % div]++;
    int best = 0;
    for (int p = 1; p < div; p++) if (count[p] > count[best]) best = p;
    return best;
}

void ScreenTap::drain_graveyard() {
    std::vector<std::shared_ptr<DecodedUnit>> drop;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (graveyard_.empty()) return;
        drop.swap(graveyard_);
        graveyard_.reserve(16);
    }
    // drop goes out of scope here, outside the lock: the pictures' MPP
    // buffers go back to the decoder.
}

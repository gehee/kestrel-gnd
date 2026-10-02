#ifndef SCREEN_TAP_HPP
#define SCREEN_TAP_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct DecodedUnit;

// What the display showed, vblank by vblank, for the screen recorder.
//
// A screen recording used to have the display controller composite the
// screen into a buffer (DRM writeback). On the goggle's kernel that costs the
// live picture: each capture is a commit of its own that holds the CRTC until
// the next frame starts, and the video's flips are refused (EBUSY) meanwhile;
// a capture carried on a video flip instead is held to the next frame
// whenever it is committed in the last eighth of one, which the renderer's
// commit just before the vblank always is. Recording dropped 8-18% of the
// pictures that should have reached the screen at 60 fps, 37% at 100 fps.
//
// So the recorder rebuilds each frame itself from the two buffers the display
// scanned out - the decoded picture and the OSD - with RGA, and never touches
// the display. This keeps track of which pair was on screen from which
// vblank: page_flip says what it is committing, its flip event on which
// vblank it landed. Only while recording; otherwise every call returns at
// once and nothing is held.
struct ScreenPair {
    std::shared_ptr<DecodedUnit> video;   // the video plane's picture, held; null if none yet
    uint32_t osd_fb  = 0;                 // the OSD plane's fb, 0 if none
    uint64_t osd_gen = 0;                 // which OSD frame is in it - the fbs are reused
    int vx = 0, vy = 0, vw = 0, vh = 0;   // the video plane's rectangle on screen
    uint32_t seq = 0;                     // the vblank it landed on
};

class ScreenTap {
public:
    ScreenTap() { graveyard_.reserve(16); }

    void set_recording(bool on);
    bool recording() const { return on_.load(std::memory_order_relaxed); }

    // page_flip, just before its commit: the pair that commit carries. A
    // commit that fails takes it back.
    void submitted(std::shared_ptr<DecodedUnit> video, uint32_t osd_fb, uint64_t osd_gen,
                   int vx, int vy, int vw, int vh, uint64_t now_us);
    void submit_failed();
    // The flip event: the pair submitted last landed on vblank seq.
    void landed(uint32_t seq);
    // A commit of the OSD plane alone (no video flowing) landed on seq.
    void osd_landed(uint32_t osd_fb, uint64_t osd_gen, uint32_t seq);

    // The pair on screen during vblank seq (at time vblank_us), copied into
    // out, which then holds the picture. A flip submitted before that vblank
    // may have landed on it with its event still on the way: wait up to
    // wait_us for it. False if nothing landed by then, or if something newer
    // already has - the caller was too late to know what seq showed.
    bool sample(uint32_t seq, uint64_t vblank_us, ScreenPair& out, int wait_us);
    // The sampled pair's OSD fb may be drawn into again.
    void sample_done();
    // Whether the recorder is reading this OSD fb right now.
    bool holds_osd(uint32_t fb);

    // The vblank parity (seq % div) most recent flips landed on, or -1.
    int dominant_parity(int div);

    // Pictures dropped from the tap are released here, by the recorder,
    // rather than in the flip handler: releasing one goes into MPP.
    void drain_graveyard();

private:
    std::atomic<bool> on_{false};
    std::mutex m_;
    std::condition_variable cv_;
    ScreenPair pending_;
    bool has_pending_ = false;
    uint64_t pending_us_ = 0;
    ScreenPair cur_;
    bool has_cur_ = false;
    uint32_t sampling_osd_ = 0;
    std::vector<std::shared_ptr<DecodedUnit>> graveyard_;
    static constexpr int kParityRing = 64;
    uint32_t land_seq_[kParityRing] = {0};
    int land_n_ = 0;
    void bury(std::shared_ptr<DecodedUnit>& du);
};

#endif

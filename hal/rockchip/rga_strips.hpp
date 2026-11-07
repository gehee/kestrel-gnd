#pragma once
// Strips joined with the RK3568's RGA2 (hal/strip_joiner.hpp): each strip's
// decoded picture is copied to its rows of a whole-picture buffer as soon as
// it is decoded, so only the last strip's copy stands between its decode and
// the screen; the buffer goes to the renderer once every strip is in. The
// buffers are the DRM device's, as the decoder's are, so the renderer and the
// screen recording take them as any decoded picture.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include "hal/strip_joiner.hpp"
#include "hal/vdec.hpp"
class RgaStripJoiner : public hal::StripJoiner {
public:
    // out: where whole pictures go (the renderer); scanout_fd: the DRM
    // device their buffers are made on.
    RgaStripJoiner(std::shared_ptr<hal::FrameSink> out, int scanout_fd);
    ~RgaStripJoiner() override;
    // Whether RGA is there to use at all (/dev/rga and the library).
    static bool available();
    void strip(int k, int n, int ov, std::shared_ptr<DecodedUnit> du) override;
    void drop() override;
private:
    static constexpr int kStrips = 4;
    // Pictures in flight: the one being joined, the one on screen, the one
    // before it (the scan-out may still read it) and a recording's.
    static constexpr int kSlots = 6;
    struct Slot {
        uint32_t gem = 0;
        int fd = -1;                 // its prime fd: the renderer's and RGA's
        uint32_t rga = 0;
        std::weak_ptr<void> out;     // the picture handed out from it, while anything holds it
    };
    struct Strip {                   // one strip's decoded picture, while it waits
        std::shared_ptr<DecodedUnit> du;
        uint64_t placed_us = 0;      // copied to its rows
    };
    struct Picture {
        int64_t pic = -1;
        int n = 0;
        int ov = 0;                  // rows each strip codes past each of its joins
        int slot = -1;
        unsigned placed = 0;         // bit k: strip k copied in
        Strip s[kStrips];
        // Strip 0's (the picture's own) figures, kept once it is copied.
        uint64_t recv_ts = 0, dec_start_ts = 0;
        uint32_t enc_us = 0, air_us = 0;
        bool key = true;
        SliceTimesPtr times[kStrips];
    };
    std::shared_ptr<hal::FrameSink> out_;
    const int drm_fd_;
    std::mutex mtx_;
    Slot slot_[kSlots];
    int next_slot_ = 0;
    uint32_t w_ = 0, h_ = 0, pitch_ = 0;   // the whole picture's
    uint32_t rows_[kStrips] = {};           // each strip's height, as last decoded
    Picture cur_;
    // A whole picture has gone out since the start (or the last drop, or the
    // air unit counting from 0 again).
    bool clean_ = false;
    // RGA's imports of the strips' decoder buffers: each strip's decoder its
    // own, released when it makes new ones (a new epoch).
    std::map<int, uint32_t> imported_[kStrips];
    uint32_t epoch_[kStrips] = {};
    unsigned long joined_ = 0, incomplete_ = 0, no_slot_ = 0;
    uint64_t copy_us_ = 0, copies_ = 0, log_ms_ = 0;
    // KESTREL_STRIP_CHECK=1: each joined picture's strips checked for being
    // of one capture - the mean luma of each strip's top and bottom halves
    // against the picture before. A brightness step (the camera's exposure
    // switched) shows in every half at once; a half that steps alone is a
    // capture other than the rest's: tearing. A bench check, off otherwise.
    bool check_ = false;
    uint8_t* map_[kSlots] = {};
    double prev_[2 * kStrips] = {};
    unsigned long checked_ = 0, steps_ = 0, torn_ = 0;
    void check_picture();
    bool alloc(uint32_t w, uint32_t h);
    void free_slots();
    int take_slot();
    uint32_t import(int k, const DecodedUnit& du);
    // Strip k's rows from `top` on, `rows` of them, to the picture's row y.
    bool copy(int k, const DecodedUnit& du, uint32_t y, uint32_t top, uint32_t rows);
    // Strip k's rows above its own (the overlap with the strip above), and
    // its own rows: the ones shown.
    uint32_t top(int k) const { return k > 0 ? (uint32_t)cur_.ov : 0; }
    uint32_t shown(int k) const {
        const uint32_t cut = top(k) + (k < cur_.n - 1 ? (uint32_t)cur_.ov : 0);
        return rows_[k] > cut ? rows_[k] - cut : 0;
    }
    // The strips of the picture being joined that can go to their rows now;
    // the whole picture out when it is complete (returned, sent unlocked).
    std::shared_ptr<DecodedUnit> place();
    std::shared_ptr<DecodedUnit> finish();
};

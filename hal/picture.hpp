#pragma once
// A decoded picture, as a decoder hands it out (hal/vdec.hpp): to the
// renderer, the screen recorder, the background video.

#include <atomic>
#include <cstdint>
#include <memory>

#include "base/slice_times.hpp"

// tx_processing_delay_us of a picture whose air part is not known (the radio
// clock not checked yet): no latency figure for it, rather than a wrong one.
static constexpr uint32_t kAirUnknown = 0xFFFFFFFFu;

struct DecodedUnit{
    uint64_t pts;
    uint32_t width;
    uint32_t height;
    uint64_t recv_ts;
    uint64_t dec_start_ts;
    uint64_t dec_end_ts;
    uint32_t tx_encode_delay_us;
    uint32_t tx_processing_delay_us;
    bool is_keyframe = false;

    // AVFrame
    // AVFrame *av_frame_ref;
    std::shared_ptr<void> frame_ref; // Holds ownership of the allocated frame (if any) with custom deleter
    uint32_t drm_pixel_format = 0;
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    uint64_t modifiers[4] = {0};

    // HW prime data
    bool has_prime_fd;
    int prime_fd;
    // The decoder's buffer behind prime_fd, and how to keep it from being
    // decoded into again: hold(buf) takes a reference, released when the
    // returned pointer goes. Nothing is held by default; the renderer holds
    // the pictures it shows while the screen is being recorded, since the
    // recorder reads them after the display does.
    void* buf = nullptr;
    std::shared_ptr<void> (*hold)(void* buf) = nullptr;
    // Bumped each time the decoder replaces its buffers: prime_fd numbers
    // are only unique within one epoch.
    uint32_t buf_epoch = 0;

    // A streamed picture shown early: only its first slice(s) are decoded yet,
    // the rest is on its way into the same buffer. bottom_eta_us says when the
    // rest should be decoded (CLOCK_MONOTONIC us, 0 = unknown), kept current by
    // the decoder; the renderer shows it only where the scan-out reaches the
    // missing part after that. The whole picture follows as usual, same pts.
    bool early = false;
    std::shared_ptr<std::atomic<uint64_t>> bottom_eta_us;
    // Each slice's own timing (base/slice_times.hpp), where the source has it.
    SliceTimesPtr slices;

    // SW data
    uint8_t *data[8];
    int linesize[8];
};

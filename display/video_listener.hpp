#pragma once
// What the renderer tells about the video it puts up, and asks of what draws
// over it (the HUD, hud/osd.hpp): the pictures' latency figures, and the
// camera's timing as the air unit reports it.

#include <cstdint>
#include <vector>

#include "base/prof.hpp"

// Each slice's own latency is kept for at most this many slices; a picture
// with more keeps this many, spread from the first to the last.
static constexpr int kLatSlices = 4;

// One slice's way through, for the life-of-a-slice chart (ms): off is its first
// row's capture after the picture's (row 0), then its stages, each with the
// share of it spent waiting (within it), and Enc's share that is the sensor
// reading the slice's rows (enc_read, within it; 0: not known), and the
// exposure that ends at its first row's capture (exp, before it; 0: not known).
struct SliceStages {
    float off = 0, enc = 0, rf = 0, dec = 0, dec_wait = 0, disp = 0, disp_wait = 0, enc_read = 0, exp = 0;
};

typedef struct {
	float min;
	float max;
	float avg;
    std::vector<float> values;
} stats;

typedef struct  {
	stats proc_latency;
	stats decoding_latency;
	stats display_latency;
    stats tx_latency;
	stats total_latency;
	stats frame_pace;
    float proc_latency_worst, decoding_latency_worst, display_latency_worst, tx_latency_worst;
} latency_stats;

typedef struct {
    float processing_ms;
    float net_ms;
    float dec_ms;
    // Each stage's share spent waiting (within it, not added): the slice
    // waiting for the rest of its picture, the decoded rows waiting for their
    // refresh. Drawn in a lighter shade.
    float processing_wait_ms = 0.0f;
    float net_wait_ms = 0.0f;
    float dec_wait_ms = 0.0f;
    float disp_wait_ms = 0.0f;
    float disp_ms;
    float pace_ms;    // Time since last frame (frame interval)
    float video_mbps; // Video bitrate at this frame
    float rf_mbps;    // RF link bitrate (data + parity) at this frame
    // For the stats screen:
    uint64_t t_us = 0;   // when this entry was made (CLOCK_MONOTONIC)
    uint32_t lost = 0;   // link packets lost so far
    int8_t   snr = 0;    // link SNR, dB
    uint8_t  mcs = 0;    // receive MCS
    int8_t   bw_idx = -1;   // the video link's bandwidth gear (gnd::bw_step_label), -1: no link
    uint8_t  air_dbm = 0;   // the air unit's TX power as its radio announced it, dBm (0: not known)
    uint16_t freq_mhz = 0;  // the link's channel, MHz (0: no link)
    int16_t  dist_m = -1;   // the HUD's distance, metres (-1: no ranging)
    uint8_t  air = 1;    // the air part is known (0: the radio clock not checked yet - no latency)
    uint8_t  key = 0;    // this picture was a keyframe
    uint16_t skipped = 0;   // pictures before this one that were never shown
    // When its top first went up (the vblank) and when it was captured, both
    // CLOCK_MONOTONIC (0: not known): the stats screen's frame spacing.
    uint64_t shown_us = 0, cap_us = 0;
    // Each slice, top to bottom, from its own first row's capture to that row
    // lit (ms); nslices 0: not timed slice by slice (see kLatSlices).
    uint8_t nslices = 0;
    float slice_ms[kLatSlices] = {0, 0, 0, 0};
    SliceStages sl[kLatSlices];   // each slice's stages and when it was captured
} LatencyFrame;

namespace display {

class VideoListener {
public:
    virtual ~VideoListener() = default;
    // A picture went up; is_keyframe for a keyframe or what stands in for one.
    virtual void notify_video_frame(bool is_keyframe = false) = 0;
    virtual void set_video_resolution(int w, int h) = 0;
    // Draw again: a new picture, or new figures about it.
    virtual void signal_render(prof::Wake why = prof::kWakeOther) = 0;
    // Every stats_period_ms, that period's figures; and each picture's.
    virtual int stats_period_ms() const = 0;
    virtual void update_stats(int current_framerate, latency_stats stats) = 0;
    virtual void add_latency_frame(LatencyFrame frame) = 0;
    // The camera's frame rate and exposure, as the air unit reports them (0:
    // not known yet).
    virtual uint32_t get_sky_exposure_us() = 0;
    virtual uint32_t get_sky_framerate() = 0;
};

}  // namespace display

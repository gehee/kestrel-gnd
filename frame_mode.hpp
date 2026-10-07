#pragma once
#include <atomic>

// How pictures go from the radio to the screen (DISPLAY > Frame Mode, saved as
// "frame_mode"; FULL FRAME by default), listed from the lowest latency to the
// highest. Read by the source, the decoder and the renderer as they go,
// so a change applies at once. The /tmp switches still override it for tests:
// kestrel-stream-off, kestrel-early-off, kestrel-split-off.
enum FrameMode {
    kFrameSplit = 0,        // each slice decoded as it lands; the top half and the
                            // lower part go up on two planes as soon as each is decoded
    kFrameWhole = 1,        // each slice decoded as it lands; only whole pictures go up
    kFrameWholeDecode = 2,  // whole pictures to the decoder, whole pictures up
};
static const int kFrameModeCount = 3;
extern std::atomic<int> g_frame_mode;

static inline const char *frame_mode_label(int m) {
    switch (m) {
        case kFrameWhole: return "FULL FRAME";
        case kFrameWholeDecode: return "FULL DECODE";
        default: return "SPLIT";
    }
}

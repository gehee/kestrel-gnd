#pragma once

#include <cstring>

enum class VideoCodec {
    UNKNOWN=0,
    H264,
    H265
};

// A picture a decoder can start at, by its NAL unit type: HEVC's random access
// points (IRAP: BLA 16-18, IDR 19-20, CRA 21), H.264's IDR (5). Every part that
// asks "is this a keyframe" - the radio's gates, the decoders, the recorders,
// the player - asks this, so what one passes the others start on.
static inline bool hevc_irap(int nal_type) { return nal_type >= 16 && nal_type <= 21; }
static inline bool keyframe_nal(VideoCodec codec, int nal_type) {
    return codec == VideoCodec::H265 ? hevc_irap(nal_type) : nal_type == 5;
}

static inline VideoCodec video_codec(const char * str) {
    if (!strcmp(str, "h264")) {
        return VideoCodec::H264;
    }
    if (!strcmp(str, "h265")) {
        return VideoCodec::H265;
    }
    return VideoCodec::UNKNOWN;
}

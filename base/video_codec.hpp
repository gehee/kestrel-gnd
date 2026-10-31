#pragma once

#include <cstring>

enum class VideoCodec {
    UNKNOWN=0,
    H264,
    H265
};

static inline VideoCodec video_codec(const char * str) {
    if (!strcmp(str, "h264")) {
        return VideoCodec::H264;
    }
    if (!strcmp(str, "h265")) {
        return VideoCodec::H265;
    }
    return VideoCodec::UNKNOWN;
}

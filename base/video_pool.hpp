#pragma once
// The pool the radio writes each picture into, once. It lives as long as the
// process: consumers (the DVR's queue) may still hold pictures when the radio
// source is gone.
#include "base/bus.hpp"

namespace video_pool {

constexpr int      kPictures = 32;            // in flight at once: the one being built + what consumers hold
// The radio's own limit on what it holds of the stream (AR_MAX_ACCUM): the
// decoder takes up to 1 MB (READ_BUF_SIZE), but the recording gets a larger
// picture all the same, as it always did. Address space only: a buffer takes
// memory as far as the largest picture written into it.
constexpr uint32_t kCapacity = 4 * 1024 * 1024;

bus::PicturePool& get();

}  // namespace video_pool

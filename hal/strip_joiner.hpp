#pragma once
// Strips: an air unit that encodes each picture as strips, each strip its own
// stream (kestrel-air's KA_SLICE_STRIP), so that most of a picture is on the
// radio before its bottom has even been encoded. Each strip is decoded on its
// own (radio/strip_video); here they are joined into one picture again, and
// only whole pictures go on to the screen: one capture on it at a time, never
// the top of one with the bottom of another.
#include <cstdint>
#include <memory>
#include "hal/picture.hpp"
namespace hal {
class StripJoiner {
public:
    virtual ~StripJoiner() = default;
    // Strip k of the n strips of picture du->pts (the air unit's picture
    // number), decoded. Strips stack top to bottom in index order, each at the
    // rows the ones above it fill. Each codes ov rows past each of its joins
    // (the air unit's overlap, so no join is at a strip's coded edge): its
    // first ov rows (but strip 0's) and its last ov (but the last strip's)
    // are not shown. A picture goes on once all n are in; one missing a strip
    // is never shown (the screen keeps the one before). From the strips'
    // decoder threads.
    virtual void strip(int k, int n, int ov, std::shared_ptr<DecodedUnit> du) = 0;
    // The picture being joined goes (the link lost, the strips stopped).
    virtual void drop() = 0;
};
}  // namespace hal

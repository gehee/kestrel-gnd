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
#include "hal/vdec.hpp"
namespace hal {
class StripJoiner {
public:
    virtual ~StripJoiner() = default;
    // Strip k of picture du->pts (the air unit's picture number), decoded;
    // last: it is the picture's bottom strip - both as its header said when
    // it arrived. Strips stack top to bottom in index order, each at the rows
    // the ones above it fill. Each codes ov rows past each of its joins (the
    // air unit's overlap, so no join is at a strip's coded edge): its first
    // ov rows (but strip 0's) and its last ov (but the last strip's) are not
    // shown. A picture goes on once all its strips are in; one missing a
    // strip is never shown (the screen keeps the one before). From the
    // strips' decoder threads.
    virtual void strip(int k, bool last, int ov, std::shared_ptr<DecodedUnit> du) = 0;
    // The picture being joined goes (the link lost, the strips stopped).
    virtual void drop() = 0;
    // A joiner that decodes the strips itself (each writing its rows of the
    // whole picture, no copy) gives strip k's decoder here; nullptr: a
    // decoder of the link's own, whose pictures come to strip().
    virtual std::unique_ptr<Vdec> decoder(int k) { (void)k; return nullptr; }
    // Strip k of picture pic arriving, before it is fed: what its header says
    // (as strip() is told once it is decoded). From the radio thread.
    virtual void arriving(int k, int64_t pic, bool last, int ov) { (void)k; (void)pic; (void)last; (void)ov; }
    // The air unit sends whole pictures again.
    virtual void stopped() {}
};
}  // namespace hal

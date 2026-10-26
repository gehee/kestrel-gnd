#pragma once
// HAL: what the display controller requires of the video planes' rectangles.
// The KMS code in drm.cpp is generic; these are the one controller's limits it
// has to keep to.

namespace hal {

struct PlaneRules {
    // Offsets and sizes of the video plane (YUV) are multiples of this, in
    // pixels and rows. A power of two.
    int align;
    // A plane with fewer rows (or columns) than this, of source or of screen,
    // is not shown at all.
    int min_rows;

    // v rounded down to the alignment.
    constexpr int down(int v) const { return v & ~(align - 1); }
    constexpr unsigned down(unsigned v) const { return v & ~(unsigned)(align - 1); }
};

// The rules of this build's display controller.
const PlaneRules &plane_rules();

}  // namespace hal

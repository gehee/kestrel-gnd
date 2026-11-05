#pragma once
#include <cstdint>
#include <stdexcept>

namespace kestrel_ui {
// Preserve the original phase, not "now + period", so polling/render jitter
// cannot accumulate. An overrun skips stale poses rather than making a burst.
inline uint64_t next_frame_deadline(uint64_t previous, uint64_t finished, uint64_t period) {
    if (!period) throw std::invalid_argument("Frame period must be nonzero");
    uint64_t next = previous + period;
    if (next <= finished) next += ((finished - next) / period + 1) * period;
    return next;
}
} // namespace kestrel_ui

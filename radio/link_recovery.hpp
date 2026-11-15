#pragma once
#include <cstdint>

namespace radio {
// A drop right after a bandwidth change that is back within this long is the
// same link re-forming, not a new one.
inline constexpr uint64_t kRetuneGraceMs = 3000;
// A bandwidth retune may briefly drop the link. If it does not return,
// resume ordinary loss recovery even when no further state event arrives.
// Only a known state counts: -1 (no reading yet, e.g. right after the socket
// was reopened) is neither down nor up.
inline bool retune_grace_expired(uint64_t now, uint64_t down_since, int state) {
    return down_since && state >= 0 && state != 2 && now >= down_since && now - down_since > kRetuneGraceMs;
}
}

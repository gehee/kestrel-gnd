#pragma once
#include <cstdint>

namespace radio {
// A bandwidth retune may briefly drop the link. If it does not return,
// resume ordinary loss recovery even when no further state event arrives.
inline bool retune_grace_expired(uint64_t now, uint64_t down_since, int state) {
    return down_since && state != 2 && now >= down_since && now - down_since > 3000;
}
}

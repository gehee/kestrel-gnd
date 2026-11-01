#pragma once
// The pictures' timing, as the radio sees them come.
#include <cstdint>

#include "system/part.hpp"

namespace gnd {

struct TimingState {
    float air_delay_ms = 0;    // capture -> arrival here (smoothed)
    bool air_delay_valid = false;
    float rsm_ms = 0;          // a picture's first slice to its last (smoothed)
};

class Timing : public Part<TimingState> {};

}  // namespace gnd

#pragma once
// The parts of the system the HUD shows and sets (system/), handed to the OSD
// when it is made: it watches those that have something to show, and calls
// them directly - nothing it calls waits on the radio.
#include "system/air/camera.hpp"
#include "system/air/encoder.hpp"
#include "system/air/fc.hpp"
#include "system/air/radio.hpp"
#include "system/air/unit.hpp"
#include "system/gnd/radio.hpp"
#include "system/gnd/timing.hpp"

namespace hud {

struct Parts {
    air::Camera& camera;
    air::Encoder& encoder;
    air::Radio& air_radio;
    air::Unit& air_unit;
    air::Fc& fc;
    gnd::Radio& radio;
    gnd::Timing& timing;
};

}  // namespace hud

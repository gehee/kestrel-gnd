#pragma once
// Which box this is, decided once at startup from the device tree.

#include <string>

#include "bsp/bsp.hpp"

namespace hal {

// The board this unit is. The first call reads /proc/device-tree/compatible
// and logs the choice; later calls return the same board.
const bsp::Board &board();

// The board a device tree's compatible list (entries joined by spaces) names,
// among the ones this build has backends for. What board() uses; exposed for
// the tests.
const bsp::Board &pick(const std::string &compat);

}  // namespace hal

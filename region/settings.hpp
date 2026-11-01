#pragma once
// The region's settings (see Setting, base/settings.hpp).

#include <string>

#include "base/settings.hpp"

namespace region::setting {

// SYSTEM > Time Zone, by its name ("" : the TZ the init script set).
inline constexpr Setting<std::string> time_zone{"time_zone", ""};
// RADIO > Amateur Licence: the amateur band's channels are usable.
inline constexpr Setting<bool> amateur_licence{"amateur_licence", false};

}  // namespace region::setting

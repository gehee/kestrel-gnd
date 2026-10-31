#pragma once
// main()'s own settings: command-line options it saves (see Setting,
// base/settings.hpp).

#include <string>

#include "base/settings.hpp"

namespace app::setting {

inline constexpr Setting<std::string> decoder_name{"decoder_name", "rkmpp"};
inline constexpr Setting<std::string> codec{"codec", ""};
inline constexpr Setting<std::string> link{"link", ""};

}  // namespace app::setting

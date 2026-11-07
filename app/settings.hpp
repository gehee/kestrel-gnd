#pragma once
// main()'s own settings (see Setting, base/settings.hpp). The command line
// overrides them for its run only. codec and link are no longer used (files
// written before may still hold them).

#include <string>

#include "base/settings.hpp"

namespace app::setting {

inline constexpr Setting<std::string> decoder_name{"decoder_name", "rkmpp"};
inline constexpr Setting<std::string> codec{"codec", ""};
inline constexpr Setting<std::string> link{"link", ""};

}  // namespace app::setting

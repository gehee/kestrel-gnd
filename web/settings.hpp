#pragma once
// The phone view's settings (see Setting, base/settings.hpp).

#include "base/settings.hpp"

namespace web::setting {

inline constexpr Setting<bool> web_stream{"web_stream", true};   // the web view runs
inline constexpr Setting<int> web_port{"web_port", 80};

}  // namespace web::setting

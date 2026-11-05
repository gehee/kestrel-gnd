#pragma once
#include "model.hpp"

namespace kestrel_ui {
UiPreferences load_preferences(const std::string& path);
void store_preferences(const std::string& path, const UiPreferences& preferences);
} // namespace kestrel_ui

#pragma once
#include "display/plane_screen.hpp"
#include <string>

namespace display {
std::shared_ptr<PlaneScreen> make_ar9311_screen(const std::string &device, bool force, volatile bool *stop, int width=1920, int height=1080);
}

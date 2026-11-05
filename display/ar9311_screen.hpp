#pragma once
#include "display/software_screen.hpp"
#include <string>

namespace display {
std::shared_ptr<SoftwareScreen> make_ar9311_screen(const std::string &device, bool force,
                                                   volatile bool *stop);
}

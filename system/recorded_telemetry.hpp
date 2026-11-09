#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace media {
// Unit-bearing keys and explicit validity flags; no local monotonic timestamps
// or native struct dumps enter the portable recording format.
struct TelemetrySample {
    std::map<std::string,double> values;
    std::map<std::string,std::string> text;
    std::array<uint16_t,53*20> grid{};
    std::vector<uint8_t> hud;
};
struct TimedTelemetry {uint64_t us=0;TelemetrySample sample;};
}

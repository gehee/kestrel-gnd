#pragma once
#include "backend_state.hpp"
#include "system/air/camera.hpp"
#include "system/air/encoder.hpp"
#include "system/air/radio.hpp"
#include "system/air/unit.hpp"
#include "system/gnd/radio.hpp"
#include <array>

namespace kestrel_ui {
// Depends on shared receiver interfaces, not an AR8030 transport or display.
class ReceiverControls {
public:
    ReceiverControls(air::Camera& c, air::Encoder& e, air::Radio& a, air::Unit& u, gnd::Radio& g)
        : camera_(c), encoder_(e), air_(a), unit_(u), ground_(g) {}
    void update(BackendState& state, uint64_t now);
    std::string apply(const std::string& id, int value, uint64_t now);
private:
    air::Camera& camera_; air::Encoder& encoder_; air::Radio& air_; air::Unit& unit_; gnd::Radio& ground_;
    std::array<uint32_t,5> versions_{};
    bool initialized_ = false, connected_ = false, service_connected_ = false;
    std::map<std::string, ReceiverControl> controls_;
};
}

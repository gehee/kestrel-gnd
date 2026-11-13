#pragma once
#include "backend_state.hpp"
#include "system/air/camera.hpp"
#include "system/air/encoder.hpp"
#include "system/air/radio.hpp"
#include "system/air/unit.hpp"
#include "system/gnd/radio.hpp"
#include <array>
#include <vector>

namespace kestrel_ui {
// Depends on shared receiver interfaces, not an AR8030 transport or display.
class ReceiverControls {
public:
    // A video mode the air unit can be asked for, at its index in the
    // radio's own table: `need` is the air feature bits it takes (0: any).
    struct VideoMode { std::string name; uint16_t need = 0; };
    // The radio's table is handed in (the app does): the menu is not built on
    // the radio.
    ReceiverControls(air::Camera& c, air::Encoder& e, air::Radio& a, air::Unit& u, gnd::Radio& g,
                     std::vector<VideoMode> video_modes = {})
        : camera_(c), encoder_(e), air_(a), unit_(u), ground_(g), video_modes_(std::move(video_modes)) {}
    void update(BackendState& state, uint64_t now);
    std::string apply(const std::string& id, int value, uint64_t now);
private:
    air::Camera& camera_; air::Encoder& encoder_; air::Radio& air_; air::Unit& unit_; gnd::Radio& ground_;
    std::vector<VideoMode> video_modes_;
    std::array<uint32_t,5> versions_{};
    bool initialized_ = false, connected_ = false, service_connected_ = false;
    std::map<std::string, ReceiverControl> controls_;
};
}

#pragma once
#include "platform/panel_logic.hpp"
#include <memory>
#include <string>
#include <vector>

namespace platform {
enum class PanelTone { Click, Notice, Success, Failure };
// Board-selected native controls. Generic evdev/control-socket input remains
// in the UI; a board without a validated hardware backend returns no events.
class NativePanel {
  public:
    virtual ~NativePanel() = default;
    virtual std::vector<PanelEvent> take() = 0;
    virtual void beep(PanelTone tone = PanelTone::Click) = 0;
    virtual std::string sources() const = 0;
};
std::unique_ptr<NativePanel> make_native_panel();
} // namespace platform

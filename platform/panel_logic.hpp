#pragma once
#include "bsp/ascent_panel.hpp"
#include <optional>

namespace platform {
using PanelKey = bsp::ascent_panel::Key;
struct PanelEvent {
    PanelKey key;
    bool repeat;
    uint64_t stamp_ms;
};
// Sample at 50 Hz. Arm only after stable idle, including after I/O errors.
// A different voltage while held cannot become another press before release.
class PanelDebouncer {
  public:
    std::optional<PanelEvent> sample(PanelKey raw, bool valid, uint64_t now) {
        if (!valid) {
            reset();
            return {};
        }
        if (have_ && now - last_sample_ > 100)
            reset();
        if (!have_ || raw != pending_) {
            pending_ = raw;
            since_ = now;
            have_ = true;
        }
        last_sample_ = now;
        if (now - since_ < 40)
            return {};
        if (raw == PanelKey::None) {
            std::optional<PanelEvent> event;
            if (held_ == PanelKey::Link)
                event =
                    PanelEvent{since_ - pressed_ >= 8000 ? PanelKey::Upgrade : PanelKey::Link, false, now};
            held_ = PanelKey::None;
            armed_ = true;
            return event;
        }
        if (armed_) {
            armed_ = false;
            held_ = raw;
            pressed_ = since_;
            next_ = now + 450;
            if (raw != PanelKey::Link)
                return PanelEvent{raw, false, now};
        } else if (raw == held_ && bsp::ascent_panel::direction(raw) && now >= next_) {
            next_ = now + 140;
            return PanelEvent{raw, true, now};
        }
        return {};
    }
    void reset() {
        have_ = armed_ = false;
        held_ = PanelKey::None;
    }

  private:
    PanelKey pending_ = PanelKey::None, held_ = PanelKey::None;
    bool have_ = false, armed_ = false;
    uint64_t since_ = 0, pressed_ = 0, next_ = 0, last_sample_ = 0;
};
} // namespace platform

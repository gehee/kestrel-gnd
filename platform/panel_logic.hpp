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
// LINK acts on release; OkBack is OK on a release before 500 ms, else Back
// once at 500 ms held. Only a board whose key map names OkBack gets that
// (bsp/ascent_panel.hpp: the SteadyDigital); every other key acts on the press.
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
            // Whichever the hold did not fire: a release right at 500 ms,
            // between two samples, is still one of the two.
            else if (held_ == PanelKey::OkBack && !held_fired_)
                event = PanelEvent{PanelKey::Ok, false, now};
            held_ = PanelKey::None;
            armed_ = true;
            return event;
        }
        if (armed_) {
            armed_ = false;
            held_ = raw;
            held_fired_ = false;
            pressed_ = since_;
            next_ = now + 450;
            if (raw != PanelKey::Link && raw != PanelKey::OkBack)
                return PanelEvent{raw, false, now};
        } else if (raw == held_ && raw == PanelKey::OkBack && now - pressed_ >= hold_ms && !held_fired_) {
            held_fired_ = true;
            return PanelEvent{PanelKey::Back, false, now};
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
    static constexpr uint64_t hold_ms = 500;
    bool held_fired_ = false;
    PanelKey pending_ = PanelKey::None, held_ = PanelKey::None;
    bool have_ = false, armed_ = false;
    uint64_t since_ = 0, pressed_ = 0, next_ = 0, last_sample_ = 0;
};
} // namespace platform

#pragma once
#include <algorithm>
#include <cstdint>

namespace kestrel_ui {
// The levels are the VRX Pro's: range_level is gnd::LinkRange::warning()
// (system/gnd/link_range.hpp: amber at 5-7 of twelve range blocks, red at 4 or
// fewer), and no frame for 400 ms on a link that was delivering them is red. A
// frame must have arrived on this connection before a stall counts. The hold
// is this frontend's own (the VRX Pro has none, only the edge's fade): worsen
// immediately, recover only after two stable seconds.
class LinkWarning {
public:
    void update(uint64_t now, bool linked, int range_level, bool monitored, uint64_t frame, bool playback) {
        if (linked && !linked_) connected_at_ = now;
        linked_ = linked;
        int raw = 0;
        if (linked) {
            raw = std::clamp(range_level, 0, 2);
            if (monitored && !playback && frame >= connected_at_ && frame && now >= frame && now - frame > 400000)
                raw = 2;
        }
        if (raw >= level_) { level_ = raw; better_at_ = 0; }
        else if (!better_at_) better_at_ = now;
        else if (now >= better_at_ && now - better_at_ >= 2000000) { level_ = raw; better_at_ = 0; }
    }
    int level() const { return level_; }
private:
    int level_ = 0;
    bool linked_ = false;
    uint64_t connected_at_ = 0, better_at_ = 0;
};
} // namespace kestrel_ui

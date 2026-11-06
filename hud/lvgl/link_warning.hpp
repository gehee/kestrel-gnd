#pragma once
#include <cstdint>

namespace kestrel_ui {
// VRX Pro policy: worsen immediately, recover only after two stable seconds.
// A frame must have arrived on this connection before a 400 ms stall counts.
class LinkWarning {
public:
    void update(uint64_t now, bool linked, int mcs, bool monitored, uint64_t frame, bool playback) {
        if (linked && !linked_) connected_at_ = now;
        linked_ = linked;
        int raw = 0;
        if (linked && mcs >= 0) {
            raw = mcs < 5 ? 2 : mcs < 8 ? 1 : 0;
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

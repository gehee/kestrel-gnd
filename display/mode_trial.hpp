#pragma once
#include <cstdint>
#include <stdexcept>

namespace display {
enum class SessionResult { Stopped, Failed, DisplayFailed };
// A trial is never persisted until explicitly confirmed. The application
// recreates its display session when restart is set, including on timeout.
class ModeTrial {
public:
    explicit ModeTrial(int height) : current(height), confirmed(height) {}
    int current, confirmed;
    bool pending = false, restart = false;
    uint64_t deadline = 0;
    void select(int height) {
        if (height != 720 && height != 1080) throw std::invalid_argument("Unsupported screen mode");
        if (height == current) return;
        current = height; pending = restart = true; deadline = 0;
    }
    void started(uint64_t now) { if (pending) deadline = now + 15000000; }
    void keep() { confirmed = current; pending = false; deadline = 0; }
    void revert() { current = confirmed; pending = false; restart = true; deadline = 0; }
    // Decoder/radio failures say nothing about the selected HDMI mode.
    bool recover(SessionResult result) {
        if (result != SessionResult::DisplayFailed) return false;
        if (pending) revert();
        else if (current != 1080) { current = confirmed = 1080; restart = true; }
        else return false;
        return true;
    }
    bool tick(uint64_t now) {
        if (!pending || !deadline || now < deadline) return false;
        revert(); return true;
    }
};
} // namespace display

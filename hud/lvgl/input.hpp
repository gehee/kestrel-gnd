#pragma once
#include "controller.hpp"
#include <csignal>
#include <sys/stat.h>
#include <termios.h>

namespace kestrel_ui {
using KeySink = std::function<void(Key, bool)>;
class TerminalDecoder {
public:
    void feed(const char* data, size_t size, uint64_t now, const KeySink& sink);
    void expire(uint64_t now, const KeySink& sink);
private:
    std::string escape_;
    uint64_t escape_at_ = 0;
    bool last_cr_ = false;
};
bool stock_key(const uint8_t* packet, size_t size, Key& key, bool& repeat);

class Input {
public:
    Input(const std::string& evdev, const std::string& control, bool stock_keys);
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    void poll(Controller& controller, uint64_t now);
    std::string sources() const;
private:
    TerminalDecoder terminal_;
    termios original_term_{};
    int original_flags_ = -1, evdev_ = -1, control_ = -1, udp_ = -1;
    bool term_saved_ = false;
    std::string control_path_;
    struct stat control_owner_{};
    void cleanup();
};
std::string send_control(const std::string& path, const std::string& command);
int interactive_control(const std::string& path, volatile sig_atomic_t& stop);
} // namespace kestrel_ui

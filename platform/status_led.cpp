#include "platform/status_led.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>

namespace platform {
namespace {
bool write_value(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size());
    close(fd);
    return ok;
}
}
StatusLed::StatusLed(const std::string& path) : path_(path) {
    std::ifstream index(path_ + "/multi_index");
    std::string r,g,b,extra;
    if (!(index >> r >> g >> b) || index >> extra || r != "red" || g != "green" || b != "blue") return;
    // Only the board's explicitly named status LED is used. Never pick an
    // arbitrary LED or probe GPIO/SPI when this interface is absent.
    if (access((path_ + "/brightness").c_str(), W_OK) ||
        access((path_ + "/multi_intensity").c_str(), W_OK)) return;
    try { worker_ = std::thread(&StatusLed::run, this); }
    catch (const std::system_error&) {
        fprintf(stderr, "status LED worker unavailable; continuing without LEDs\n");
    }
}
StatusLed::~StatusLed() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
bool StatusLed::write_color(const StatusColor& color) {
    return write_value(path_ + "/multi_intensity", std::to_string(color[0])+" "+
        std::to_string(color[1])+" "+std::to_string(color[2])+"\n");
}
void StatusLed::run() {
    using namespace std::chrono;
    auto epoch = steady_clock::now();
    StatusColor previous{999,999,999};
    bool enabled = false;
    for (;;) {
        { std::lock_guard<std::mutex> lock(mutex_); if (stop_) break; }
        auto ms = duration_cast<milliseconds>(steady_clock::now()-epoch).count();
        auto color = status_color(state_.load(std::memory_order_relaxed), ms);
        if (color != previous) {
            if (!write_color(color) || (!enabled && !write_value(path_+"/brightness", "255\n"))) {
                fprintf(stderr, "status LED unavailable; disabling status output\n");
                break;
            }
            enabled = true;
            previous = color;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, milliseconds(100), [this]{return stop_;})) break;
    }
    write_value(path_+"/brightness", "0\n");
}
}

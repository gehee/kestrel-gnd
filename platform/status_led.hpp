#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace platform {
enum class ReceiverStatus { Starting, NoVideo, Pairing, Video, Recording, Fault };
using StatusColor = std::array<unsigned, 3>;
inline StatusColor status_color(ReceiverStatus state, uint64_t ms) {
    const StatusColor off{0,0,0}, red{255,0,0}, green{0,255,0}, blue{0,0,255};
    switch (state) {
    case ReceiverStatus::Starting: return ms % 2000 < 1000 ? green : off;
    case ReceiverStatus::NoVideo: return red;
    case ReceiverStatus::Pairing: return ms % 600 < 300 ? blue : off;
    case ReceiverStatus::Video: return green;
    case ReceiverStatus::Recording: return ms % 1000 < 200 ? blue : green;
    case ReceiverStatus::Fault: return ms % 400 < 200 ? red : off;
    }
    return off;
}
inline ReceiverStatus receiver_status(bool fault, bool pairing, bool fresh, bool recording) {
    if (fault) return ReceiverStatus::Fault;
    if (pairing) return ReceiverStatus::Pairing;
    if (!fresh) return ReceiverStatus::NoVideo;
    return recording ? ReceiverStatus::Recording : ReceiverStatus::Video;
}
// Optional Linux multicolor LED. Missing hardware means no thread and no writes.
// Video/UI threads only publish an atomic state; sysfs I/O stays on the worker.
class StatusLed {
public:
    explicit StatusLed(const std::string& path = "/sys/class/leds/fpvos:status");
    ~StatusLed();
    StatusLed(const StatusLed&) = delete;
    StatusLed& operator=(const StatusLed&) = delete;
    void set(ReceiverStatus state) { state_.store(state, std::memory_order_relaxed); }
private:
    void run();
    bool write_color(const StatusColor& color);
    std::string path_;
    std::atomic<ReceiverStatus> state_{ReceiverStatus::Starting};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
};
}

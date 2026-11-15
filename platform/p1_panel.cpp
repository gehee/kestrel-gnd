#include "platform/native_panel.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace platform {
namespace {
PanelKey mapped(unsigned code) {
    switch (code) {
    case KEY_UP: return PanelKey::Up;
    case KEY_DOWN: return PanelKey::Down;
    case KEY_LEFT: return PanelKey::Left;
    case KEY_RIGHT: return PanelKey::Right;
    case KEY_ENTER: return PanelKey::Ok;
    case KEY_BACK: return PanelKey::Back;
    case KEY_RECORD: return PanelKey::Rec;
    case KEY_CONNECT: return PanelKey::Link;
    default: return PanelKey::None;
    }
}
class P1Panel final : public NativePanel {
    int fd_ = -1;
    PanelDebouncer debounce_;
    std::string path_;
  public:
    P1Panel() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/input", ec)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("event", 0) != 0) continue;
            std::ifstream file(entry.path() / "device/name");
            std::string device; std::getline(file, device);
            if (device != "p1-goggle-keys") continue;
            path_ = "/dev/input/" + name;
            fd_ = open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) != 0) {
                close(fd_); fd_ = -1;
            }
            break;
        }
        printf("panel: P1 evdev=%s\n", fd_ >= 0 ? path_.c_str() : "unavailable");
    }
    ~P1Panel() override { if (fd_ >= 0) close(fd_); }
    std::vector<PanelEvent> take() override {
        if (fd_ < 0) return {};
        // Drain events, then query the current bitmap. This also recovers from
        // SYN_DROPPED and prevents queued repeats firing after a UI stall.
        input_event event{};
        while (read(fd_, &event, sizeof(event)) == sizeof(event)) {}
        unsigned char keys[(KEY_MAX + 8) / 8]{};
        const bool valid = ioctl(fd_, EVIOCGKEY(sizeof(keys)), keys) >= 0;
        PanelKey key = PanelKey::None;
        for (unsigned code = 0; code <= KEY_MAX; ++code) {
            if (!(keys[code / 8] & (1u << (code % 8)))) continue;
            const auto next = mapped(code);
            if (next == PanelKey::None) continue;
            if (key != PanelKey::None) { debounce_.reset(); return {}; }
            key = next;
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (auto e = debounce_.sample(key, valid, now)) return {*e};
        return {};
    }
    void beep(PanelTone) override {} // No validated beeper on P1 goggles.
    std::string sources() const override { return fd_ >= 0 ? ", P1 keypad" : ""; }
};
}
std::unique_ptr<NativePanel> make_native_panel() { return std::make_unique<P1Panel>(); }
} // namespace platform

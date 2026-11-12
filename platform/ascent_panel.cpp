#include "platform/ascent_panel.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <mutex>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace platform {
namespace {
namespace board = bsp::ascent_panel;
using Clock = std::chrono::steady_clock;
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
struct Fd {
    int value = -1;
    ~Fd() {
        if (value >= 0)
            close(value);
    }
};
// 0, or the errno of the failed open/write.
int write_error(const std::string &path, unsigned value) {
    Fd fd{open(path.c_str(), O_WRONLY | O_CLOEXEC)};
    if (fd.value < 0)
        return errno;
    const auto text = std::to_string(value);
    const ssize_t n = write(fd.value, text.data(), text.size());
    return n == ssize_t(text.size()) ? 0 : n < 0 ? errno : EIO;
}
bool write_value(const std::string &path, unsigned value) { return write_error(path, value) == 0; }
// Device-tree compatible list (NUL-separated); the panel is board wiring.
bool board_is(const char *compatible) {
    std::ifstream file("/proc/device-tree/compatible", std::ios::binary);
    const std::string list((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (size_t at = 0; at < list.size();) {
        const size_t end = list.find('\0', at);
        if (list.compare(at, (end == std::string::npos ? list.size() : end) - at, compatible) == 0)
            return true;
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return false;
}
// The evdev node whose device name matches, opened read-only.
int open_evdev(const char *name) {
    for (unsigned i = 0; i < 32; ++i) {
        const int fd = open(("/dev/input/event" + std::to_string(i)).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char got[64]{};
        if (ioctl(fd, EVIOCGNAME(sizeof(got) - 1), got) >= 0 && std::string(got) == name)
            return fd;
        close(fd);
    }
    return -1;
}
bool key_down(const unsigned long *bits, unsigned code) {
    const unsigned width = 8 * sizeof(unsigned long);
    return (bits[code / width] >> (code % width)) & 1;
}
// The SteadyDigital's keys, one at a time as the Gnd485's ADC reports its
// own: the first held in the board's order wins.
PanelKey steadydigital_key(int fd, bool &ok) {
    unsigned long bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))]{};
    ok = ioctl(fd, EVIOCGKEY(sizeof(bits)), bits) >= 0;
    if (!ok)
        return PanelKey::None;
    for (const auto &button : board::steadydigital::buttons)
        if (key_down(bits, button.code))
            return button.key;
    return PanelKey::None;
}
struct Pwm {
    std::string chip, channel;
    bool owned = false;
    ~Pwm() {
        if (owned) {
            off();
            if (!write_value(chip + "/unexport", board::gnd485::pwm_channel))
                fprintf(stderr, "panel: PWM unexport failed\n");
        }
    }
    bool off() {
        const bool ok = write_value(channel + "/enable", 0);
        if (!ok)
            fprintf(stderr, "panel: PWM disable failed\n");
        return ok;
    }
    // reclaim: this process holds the panel's lock (the ADC's flock), which no
    // other kestrel-gnd can, so a channel found exported is a dead run's.
    bool start(bool reclaim) {
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator("/sys/class/pwm", ec)) {
            auto device = std::filesystem::canonical(entry.path() / "device", ec);
            if (ec || device.filename() != board::gnd485::pwm_controller)
                continue;
            chip = entry.path();
            channel = chip + "/pwm" + std::to_string(board::gnd485::pwm_channel);
            // An existing export belongs to someone else; never take it over -
            // unless it is an earlier kestrel-gnd's: a crash or a SIGKILL
            // leaves the channel exported with nobody to unexport it, and the
            // beeper stayed silent until a reboot.
            const int err = write_error(chip + "/export", board::gnd485::pwm_channel);
            if (err == EBUSY && reclaim && std::filesystem::is_directory(channel, ec))
                fprintf(stderr, "panel: reclaiming the beeper PWM an earlier run left exported\n");
            else if (err)
                return false;
            owned = true;
            return off() && write_value(channel + "/duty_cycle", 0) &&
                   write_value(channel + "/period", board::gnd485::period_ns) &&
                   write_value(channel + "/duty_cycle", board::gnd485::duty_ns);
        }
        return false;
    }
};
} // namespace

struct AscentPanel::Impl {
    Fd adc, gpio, keys;
    Pwm pwm;
    bool pwm_ready = false;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    std::vector<PanelEvent> events;
    std::optional<PanelTone> tone;
    std::thread inputs, sound;
    Impl() {
        if (board_is(board::steadydigital::compatible)) {
            keys.value = open_evdev(board::steadydigital::keys_device);
            // Another kestrel-gnd holding the keys gets them, as with the ADC.
            if (keys.value >= 0 && flock(keys.value, LOCK_EX | LOCK_NB) != 0) {
                close(keys.value);
                keys.value = -1;
            }
            printf("panel: SteadyDigital keys=%s\n", keys.value >= 0 ? "ready" : "unavailable");
        } else if (!board_is(board::gnd485::compatible)) {
            printf("panel: no front panel known for this board\n");
        } else {
            open_gnd485();
        }
        try {
            inputs = std::thread([this] { poll(); });
            if (pwm_ready)
                sound = std::thread([this] { play(); });
        } catch (...) {
            finish();
            throw;
        }
    }
    void open_gnd485() {
        adc.value = open(board::gnd485::adc_device, O_RDWR | O_CLOEXEC);
        // Prevent another Kestrel instance consuming the same ADC presses.
        if (adc.value >= 0 && flock(adc.value, LOCK_EX | LOCK_NB) != 0) {
            close(adc.value);
            adc.value = -1;
        }
        for (unsigned i = 0; i < 64; ++i) {
            Fd chip{open(("/dev/gpiochip" + std::to_string(i)).c_str(), O_RDONLY | O_CLOEXEC)};
            gpiochip_info info{};
            if (chip.value < 0 || ioctl(chip.value, GPIO_GET_CHIPINFO_IOCTL, &info) < 0 ||
                std::string(info.label) != board::gnd485::gpio_label)
                continue;
            gpiohandle_request request{};
            request.lineoffsets[0] = board::gnd485::link_offset;
            request.flags = GPIOHANDLE_REQUEST_INPUT;
            request.lines = 1;
            strcpy(request.consumer_label, "kestrel-panel");
            if (ioctl(chip.value, GPIO_GET_LINEHANDLE_IOCTL, &request) == 0)
                gpio.value = request.fd;
            break;
        }
        pwm_ready = pwm.start(adc.value >= 0);
        printf("panel: ADC=%s LINK=%s beeper=%s\n", adc.value >= 0 ? "ready" : "unavailable",
               gpio.value >= 0 ? "ready" : "unavailable", pwm_ready ? "ready" : "unavailable");
    }
    ~Impl() { finish(); }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        // Sound stops independently, even if an I2C transaction is timing out.
        if (sound.joinable())
            sound.join();
        if (inputs.joinable())
            inputs.join();
    }
    void poll() {
        PanelDebouncer adc_keys, link, gpio_keys;
        bool adc_failed = false, gpio_failed = false, keys_failed = false;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (wake.wait_for(lock, std::chrono::milliseconds(20), [this] { return stop; }))
                    return;
            }
            if (adc.value >= 0) {
                uint8_t reg = board::gnd485::adc_register, bytes[2]{};
                i2c_msg messages[] = {{board::gnd485::adc_address, 0, 1, &reg},
                                      {board::gnd485::adc_address, I2C_M_RD, 2, bytes}};
                i2c_rdwr_ioctl_data transfer{messages, 2};
                const bool ok = ioctl(adc.value, I2C_RDWR, &transfer) == 2;
                report(ok, adc_failed, "ADC");
                push(adc_keys.sample(board::gnd485::decode(uint16_t(bytes[0]) << 8 | bytes[1]), ok, now_ms()));
            }
            if (keys.value >= 0) {
                bool ok = false;
                const PanelKey key = steadydigital_key(keys.value, ok);
                report(ok, keys_failed, "keys");
                push(gpio_keys.sample(key, ok, now_ms()));
            }
            if (gpio.value >= 0) {
                gpiohandle_data data{};
                const bool ok = ioctl(gpio.value, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) == 0;
                report(ok, gpio_failed, "LINK");
                push(link.sample(data.values[0] ? PanelKey::None : PanelKey::Link, ok, now_ms()));
            }
        }
    }
    static void report(bool ok, bool &failed, const char *source) {
        if (failed == ok)
            fprintf(stderr, "panel: %s read %s\n", source, ok ? "recovered" : "failed");
        failed = !ok;
    }
    void push(std::optional<PanelEvent> event) {
        if (!event)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (events.size() < 32)
            events.push_back(*event);
    }
    void play() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stop) {
            wake.wait(lock, [this] { return stop || tone.has_value(); });
            if (stop)
                break;
            const auto current = *tone;
            tone.reset();
            // Notifications preempt clicks; clicks during a notification are coalesced.
            const int count = current == PanelTone::Success ? 3 : current == PanelTone::Notice ? 2 : 1;
            for (int n = 0; n < count && !stop; ++n) {
                const int length =
                    current == PanelTone::Click                                                    ? 60
                    : (current == PanelTone::Failure || (current == PanelTone::Success && n == 2)) ? 500
                                                                                                   : 150;
                if (!write_value(pwm.channel + "/enable", 1)) {
                    stop_sound();
                    return;
                }
                const bool interrupted = wake.wait_for(lock, std::chrono::milliseconds(length), [this] {
                    return stop || (tone && *tone != PanelTone::Click);
                });
                if (!pwm.off()) {
                    stop_sound();
                    return;
                }
                if (interrupted)
                    break;
                if (n + 1 < count && wake.wait_for(lock, std::chrono::milliseconds(350), [this] {
                        return stop || (tone && *tone != PanelTone::Click);
                    }))
                    break;
            }
        }
        pwm.off();
    }
    void stop_sound() {
        pwm.off();
        pwm_ready = false;
        tone.reset();
    }
};
AscentPanel::AscentPanel() : impl_(std::make_unique<Impl>()) {}
AscentPanel::~AscentPanel() = default;
std::vector<PanelEvent> AscentPanel::take() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<PanelEvent> result;
    const auto now = now_ms();
    for (auto event : impl_->events)
        if (now - event.stamp_ms <= 250)
            result.push_back(event);
    impl_->events.clear();
    return result;
}
void AscentPanel::beep(PanelTone tone) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->pwm_ready || impl_->stop)
        return;
    if (!impl_->tone || tone != PanelTone::Click)
        impl_->tone = tone;
    impl_->wake.notify_all();
}
std::string AscentPanel::sources() const {
    return std::string(impl_->adc.value >= 0 ? ", panel ADC" : "") +
           (impl_->gpio.value >= 0 ? ", LINK GPIO" : "") + (impl_->keys.value >= 0 ? ", panel keys" : "");
}
} // namespace platform

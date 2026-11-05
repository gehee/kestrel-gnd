#include "input.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace kestrel_ui {
namespace {
sockaddr_un address(const std::string& path) {
    sockaddr_un addr{}; addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) throw std::invalid_argument("Invalid local control socket path");
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}
bool same_inode(const struct stat& a, const struct stat& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
void prepare_control_path(const std::string& path) {
    struct stat before{};
    if (lstat(path.c_str(), &before)) {
        if (errno == ENOENT) return;
        throw std::runtime_error("Cannot inspect local control socket");
    }
    if (!S_ISSOCK(before.st_mode) || before.st_uid != geteuid()) throw std::runtime_error("Control path exists and is not our socket");
    const int probe = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (probe < 0) throw std::runtime_error("Cannot probe control socket");
    const auto addr = address(path);
    const int connected = connect(probe, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    const int error = errno; close(probe);
    if (!connected || error != ECONNREFUSED) throw std::runtime_error("Local control socket already active");
    struct stat after{};
    if (lstat(path.c_str(), &after) || !same_inode(before, after) || unlink(path.c_str()))
        throw std::runtime_error("Cannot remove stale owned control socket");
}
}
void TerminalDecoder::feed(const char* data, size_t size, uint64_t now, const KeySink& sink) {
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = data[i];
        if (last_cr_ && c == '\n') { last_cr_ = false; continue; }
        last_cr_ = c == '\r';
        if (!escape_.empty()) {
            if (escape_.size() == 1 && (c == '[' || c == 'O')) { escape_ += char(c); continue; }
            if (escape_.size() > 1) {
                if ((c >= '0' && c <= '9') || c == ';') {
                    if (escape_.size() < 16) escape_ += char(c);
                    else escape_.clear();
                    continue;
                }
                if (c == 'A') sink(Key::Up, false);
                if (c == 'B') sink(Key::Down, false);
                if (c == 'C') sink(Key::Right, false);
                if (c == 'D') sink(Key::Left, false);
                escape_.clear(); continue;
            }
            sink(Key::Back, false); escape_.clear();
        }
        if (c == 27) { escape_ = "\x1b"; escape_at_ = now; }
        else if (c == 'm' || c == 'M') sink(Key::Menu, false);
        else if (c == 'w' || c == 'W') sink(Key::Up, false);
        else if (c == 's' || c == 'S') sink(Key::Down, false);
        else if (c == 'a' || c == 'A') sink(Key::Left, false);
        else if (c == 'd' || c == 'D' || c == '\t') sink(Key::Right, false);
        else if (c == '\n' || c == '\r') sink(Key::Enter, false);
        else if (c == 8 || c == 127) sink(Key::Back, false);
        else if (c == 'q' || c == 'Q') sink(Key::Quit, false);
    }
}
void TerminalDecoder::expire(uint64_t now, const KeySink& sink) {
    if (!escape_.empty() && now >= escape_at_ && now - escape_at_ > 150000) {
        if (escape_.size() == 1) sink(Key::Back, false);
        escape_.clear();
    }
}
bool stock_key(const uint8_t* packet, size_t size, Key& key, bool& repeat) {
    // GlassesUI KeyThread reads [source, key ID, event] from loopback :6222.
    // Only normalized menu keys from source 0x40 are admitted. Long/debug,
    // attitude, record and pairing events are not translated into UI actions.
    if (size != 3 || packet[0] != 0x40 || (packet[2] != 0 && packet[2] != 2)) return false;
    switch (packet[1]) {
        case 0: key = Key::Up; break;
        case 1: key = Key::Down; break;
        case 2: key = Key::Left; break;
        case 3: key = Key::Right; break;
        case 4: key = Key::Enter; break;
        case 7: key = Key::Back; break;
        default: return false;
    }
    repeat = packet[2] == 2;
    return !repeat || (key != Key::Enter && key != Key::Back);
}
Input::Input(const std::string& evdev, const std::string& control, bool stock_keys) : control_path_(control) {
    try {
        original_flags_ = fcntl(STDIN_FILENO, F_GETFL);
        if (original_flags_ >= 0) fcntl(STDIN_FILENO, F_SETFL, original_flags_ | O_NONBLOCK);
        if (isatty(STDIN_FILENO) && !tcgetattr(STDIN_FILENO, &original_term_)) {
            auto raw = original_term_; raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
            term_saved_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
        if (!evdev.empty()) {
            evdev_ = open(evdev.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (evdev_ < 0) throw std::runtime_error("Cannot open evdev input");
        }
        if (!control.empty()) {
            prepare_control_path(control);
            control_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            const auto addr = address(control);
            if (control_ < 0 || bind(control_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)))
                throw std::runtime_error("Cannot bind local control socket");
            if (lstat(control.c_str(), &control_owner_) || chmod(control.c_str(), 0600))
                throw std::runtime_error("Cannot secure local control socket");
            std::printf("INPUT control=%s\n", control.c_str());
        }
        if (stock_keys) {
            udp_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(6222);
            if (udp_ < 0 || bind(udp_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)))
                throw std::runtime_error("Cannot bind stock key port 127.0.0.1:6222");
            std::puts("INPUT stock_keys=127.0.0.1:6222");
        }
    } catch (...) { cleanup(); throw; }
}
Input::~Input() { cleanup(); }
void Input::cleanup() {
    if (term_saved_) { tcsetattr(STDIN_FILENO, TCSANOW, &original_term_); term_saved_ = false; }
    if (original_flags_ >= 0) { fcntl(STDIN_FILENO, F_SETFL, original_flags_); original_flags_ = -1; }
    for (int* fd : {&evdev_, &control_, &udp_}) if (*fd >= 0) { close(*fd); *fd = -1; }
    if (!control_path_.empty() && control_owner_.st_ino) {
        struct stat current{};
        if (!lstat(control_path_.c_str(), &current) && same_inode(current, control_owner_)) unlink(control_path_.c_str());
        control_owner_.st_ino = 0;
    }
}
std::string Input::sources() const {
    std::string result = isatty(STDIN_FILENO) ? "terminal" : "stdin";
    if (evdev_ >= 0) result += ", evdev";
    if (control_ >= 0) result += ", local control";
    if (udp_ >= 0) result += ", stock keys";
    return result;
}
void Input::poll(Controller& controller, uint64_t now) {
    auto& model = controller.model();
    const KeySink sink = [&](Key k, bool repeated) { controller.key(k, repeated); };
    char bytes[64];
    const ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
    if (n > 0) terminal_.feed(bytes, size_t(n), now, sink);
    terminal_.expire(now, sink);
    input_event event{};
    for (int count = 0; evdev_ >= 0 && count < 32 && read(evdev_, &event, sizeof(event)) == sizeof(event); ++count) {
        if (event.type != EV_KEY || event.value < 1 || event.value > 2) continue;
        Key key;
        switch (event.code) {
            case KEY_UP: key = Key::Up; break;
            case KEY_DOWN: key = Key::Down; break;
            case KEY_LEFT: key = Key::Left; break;
            case KEY_RIGHT: case KEY_TAB: key = Key::Right; break;
            case KEY_ENTER: case KEY_KPENTER: key = Key::Enter; break;
            case KEY_ESC: case KEY_BACK: case KEY_BACKSPACE: key = Key::Back; break;
            case KEY_M: case KEY_MENU: key = Key::Menu; break;
            case KEY_Q: key = Key::Quit; break;
            default: continue;
        }
        controller.key(key, event.value == 2);
    }
    for (int count = 0; udp_ >= 0 && count < 8; ++count) {
        uint8_t packet[4];
        const ssize_t size = recv(udp_, packet, sizeof(packet), MSG_TRUNC);
        if (size < 0) break;
        Key key; bool repeat;
        if (stock_key(packet, size_t(size), key, repeat)) controller.key(key, repeat);
        else ++model.rejected_inputs;
    }
    for (int count = 0; control_ >= 0 && count < 8; ++count) {
        sockaddr_un peer{}; socklen_t length = sizeof(peer);
        char data[64];
        const ssize_t size = recvfrom(control_, data, sizeof(data), MSG_TRUNC, reinterpret_cast<sockaddr*>(&peer), &length);
        if (size < 0) break;
        std::string response = "ERR invalid command";
        if (size > 0 && size <= 32) {
            const std::string command(data, size_t(size));
            Key key;
            if (command == "status") response = model.describe();
            else if (parse_key(command, key)) { controller.key(key); response = model.describe(); }
            else ++model.rejected_inputs;
        } else ++model.rejected_inputs;
        if (length > offsetof(sockaddr_un, sun_path) && peer.sun_path[0])
            sendto(control_, response.data(), response.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&peer), length);
    }
}
std::string send_control(const std::string& path, const std::string& command) {
    if (command != "status") { Key key; if (!parse_key(command, key)) throw std::invalid_argument("Unknown menu key"); }
    const auto destination = address(path);
    const auto slash = path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
    std::string pattern = directory + "/.kestrel-ui-client-XXXXXX";
    std::vector<char> file(pattern.begin(), pattern.end()); file.push_back('\0');
    int temporary = mkstemp(file.data());
    if (temporary < 0) throw std::runtime_error("Cannot create local control client");
    close(temporary); unlink(file.data());
    const std::string own_path(file.data());
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    try {
        const auto own = address(own_path);
        if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr*>(&own), sizeof(own)) || chmod(own_path.c_str(), 0600) ||
            connect(fd, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)))
            throw std::runtime_error("Cannot connect to KestrelUI control socket");
        if (send(fd, command.data(), command.size(), MSG_NOSIGNAL) != ssize_t(command.size())) throw std::runtime_error("Control send failed");
        pollfd ready{fd, POLLIN, 0};
        int waited;
        do { waited = ::poll(&ready, 1, 1000); } while (waited < 0 && errno == EINTR);
        if (waited <= 0) throw std::runtime_error("KestrelUI control response timed out");
        char response[512]; const ssize_t n = recv(fd, response, sizeof(response), 0);
        if (n <= 0) throw std::runtime_error("Invalid KestrelUI control response");
        close(fd); unlink(own_path.c_str());
        return std::string(response, size_t(n));
    } catch (...) { if (fd >= 0) close(fd); unlink(own_path.c_str()); throw; }
}
int interactive_control(const std::string& path, volatile sig_atomic_t& stop) {
    std::puts("KestrelUI controller: Enter/M opens, arrows/WASD navigate, Esc backs out, Q exits this controller.");
    std::puts(send_control(path, "status").c_str());
    Input terminal("", "", false);
    TerminalDecoder decoder;
    bool done = false;
    const KeySink sink = [&](Key key, bool) {
        if (key == Key::Quit) { done = true; return; }
        std::puts(send_control(path, key_name(key)).c_str());
    };
    while (!stop && !done) {
        char data[64]; const ssize_t n = read(STDIN_FILENO, data, sizeof(data));
        const uint64_t now = monotonic_us();
        if (n > 0) decoder.feed(data, size_t(n), now, sink);
        else if (n == 0 && !isatty(STDIN_FILENO)) break;
        decoder.expire(now, sink); usleep(10000);
    }
    return 0;
}
} // namespace kestrel_ui

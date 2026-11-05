#include "preferences.hpp"
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace kestrel_ui {
UiPreferences load_preferences(const std::string& path) {
    UiPreferences result;
    if (path.empty()) return result;
    std::ifstream input(path);
    if (!input) {
        if (errno == ENOENT) return result;
        throw std::runtime_error("Cannot read UI preferences: " + path);
    }
    std::string line;
    if (!std::getline(input, line) || line != "KESTREL_UI_SETTINGS=1") throw std::runtime_error("Unrecognized UI preferences file; left untouched");
    int lines = 0;
    while (std::getline(input, line)) {
        if (++lines > 4 || line.size() > 64) throw std::runtime_error("Invalid UI preferences size");
        const auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Invalid UI preference");
        const auto key = line.substr(0, equal), raw = line.substr(equal + 1);
        size_t used = 0; const int value = std::stoi(raw, &used);
        if (used != raw.size()) throw std::runtime_error("Invalid UI preference value");
        if (key == "theme") result.theme = value;
        else if (key == "fps") result.fps = value;
        else if (key == "details" && (value == 0 || value == 1)) result.details = value != 0;
        else if (key == "repeat" && (value == 0 || value == 1)) result.repeat = value != 0;
        else throw std::runtime_error("Unknown UI preference; file left untouched");
    }
    Model validate; validate.set_preferences(result);
    return result;
}
void store_preferences(const std::string& path, const UiPreferences& p) {
    Model validate; validate.set_preferences(p);
    const auto slash = path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
    std::string temporary = directory + "/.kestrel-ui-settings-XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end()); name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) throw std::runtime_error(std::strerror(errno));
    try {
        fchmod(fd, 0600);
        const std::string data = "KESTREL_UI_SETTINGS=1\ntheme=" + std::to_string(p.theme) + "\nfps=" + std::to_string(p.fps) +
                                 "\ndetails=" + std::to_string(p.details) + "\nrepeat=" + std::to_string(p.repeat) + "\n";
        size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t n = write(fd, data.data() + offset, data.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Writing UI preferences failed");
            offset += size_t(n);
        }
        if (fsync(fd)) throw std::runtime_error("Syncing UI preferences failed");
        close(fd); fd = -1;
        if (rename(name.data(), path.c_str())) throw std::runtime_error("Replacing UI preferences failed");
    } catch (...) {
        if (fd >= 0) close(fd);
        unlink(name.data()); throw;
    }
}
} // namespace kestrel_ui

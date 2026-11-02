#include "base/settings.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

std::string Settings::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

void Settings::load(const std::string& filename) {
    std::lock_guard<std::mutex> lock(_mutex);
    _filename = filename;
    _settings.clear();
    _saved_gen = _gen;   // what is in memory now is what the file holds

    std::ifstream file(filename);
    if (!file.is_open()) {
        // It's okay if file doesn't exist, we'll just use defaults and create it later
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = trim(line.substr(0, colonPos));
            std::string value = trim(line.substr(colonPos + 1));
            _settings[key] = value;
        }
    }
}

// The time a change waits for others to share its write. A menu row being
// scrolled, or the display lock settling a camera period, is a burst of sets;
// one write covers them.
static constexpr auto kSaveDelay = std::chrono::milliseconds(200);

Settings::~Settings() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _saver_stop = true;
    }
    _saver_cv.notify_all();
    if (_saver.joinable()) _saver.join();
    flush();
}

void Settings::mark_dirty() {
    // The first change of a burst sets the deadline; later ones ride on it,
    // so a steady stream of sets still reaches the disk every kSaveDelay.
    // After a failed write, the next change starts a burst of its own.
    if (_gen == _saved_gen || _gen == _failed_gen) _due = std::chrono::steady_clock::now() + kSaveDelay;
    _gen++;
    if (!_saver.joinable() && !_saver_stop) _saver = std::thread(&Settings::saver_run, this);
    _saver_cv.notify_one();
}

void Settings::saver_run() {
    // Started by whichever thread set something first, maybe a real-time one
    // (the display lock, the video thread), whose policy it would inherit.
    pthread_setname_np(pthread_self(), "settings");
    struct sched_param sp = {};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    std::unique_lock<std::mutex> lock(_mutex);
    while (!_saver_stop) {
        if (_gen == _saved_gen || _gen == _failed_gen) {   // nothing new to write
            _saver_cv.wait(lock);
            continue;
        }
        if (_saver_cv.wait_until(lock, _due) != std::cv_status::timeout && !_saver_stop) continue;
        if (_saver_stop) break;
        lock.unlock();
        write_snapshot();
        lock.lock();
        // Changes made while the file was being written get a delay of their own.
        if (_gen != _saved_gen && _due <= std::chrono::steady_clock::now())
            _due = std::chrono::steady_clock::now() + kSaveDelay;
    }
}

void Settings::write_snapshot() {
    std::lock_guard<std::mutex> wlock(_write_mutex);
    std::map<std::string, std::string> snap;
    std::string filename;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_gen == _saved_gen || _filename.empty()) return;
        snap = _settings;
        filename = _filename;
        gen = _gen;
    }

    std::string text;
    for (const auto& pair : snap) text += pair.first + ": " + pair.second + "\n";
    std::string err;
    const bool ok = write_file(filename, text, err);

    // A failed write stays unsaved, so the next change (or a flush()) tries
    // again; said once, not at every change of a read-only file.
    std::lock_guard<std::mutex> lock(_mutex);
    if (ok) {
        if (gen > _saved_gen) _saved_gen = gen;
        if (_write_failed) std::cerr << "settings: " << filename << " written again" << std::endl;
        _write_failed = false;
    } else {
        if (!_write_failed) std::cerr << "settings: cannot write " << filename << ": " << err << std::endl;
        _write_failed = true;
        _failed_gen = gen;
    }
}

// A new file beside the real one, on disk, then renamed over it: a power cut,
// a kill or a full disk mid-write leaves the old file whole, never an empty
// or a partial one. A link is followed first (the file may be a link into a
// writable partition), so the link stays and its target is what is replaced.
bool Settings::write_file(const std::string& filename, const std::string& text, std::string& err) {
    std::string target = filename;
    if (char* real = realpath(filename.c_str(), nullptr)) {
        target = real;
        free(real);
    } else {
        // Not there yet; or a link to a file not made yet, which is made.
        char link[PATH_MAX];
        const ssize_t n = readlink(filename.c_str(), link, sizeof link - 1);
        if (n > 0) {
            link[n] = 0;
            const size_t slash = filename.rfind('/');
            target = link[0] == '/' || slash == std::string::npos
                ? std::string(link) : filename.substr(0, slash + 1) + link;
        }
    }
    const size_t slash = target.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : target.substr(0, slash);
    const std::string tmp = target + ".tmp";
    struct stat st;
    const mode_t mode = stat(target.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0644;

    // On any failure: why, and no .tmp left behind.
    auto fail = [&](const char* what, int fd) {
        err = std::string(what) + " " + tmp + ": " + strerror(errno);
        if (fd >= 0) close(fd);
        unlink(tmp.c_str());
        return false;
    };
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return fail("open", -1);
    for (size_t w = 0; w < text.size();) {
        const ssize_t r = ::write(fd, text.data() + w, text.size() - w);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (r == 0) errno = ENOSPC;
            return fail("write", fd);
        }
        w += (size_t)r;
    }
    if (fsync(fd) != 0) return fail("fsync", fd);
    if (close(fd) != 0) return fail("close", -1);
    if (rename(tmp.c_str(), target.c_str()) != 0) return fail("rename", -1);
    // The rename on disk too. Not a failure when it cannot be (a filesystem
    // that does not sync directories): the new file is in place either way,
    // and at worst a power cut brings back the old one, whole.
    const int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

void Settings::flush() {
    write_snapshot();
}

std::string Settings::all_text() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string text;
    for (const auto& pair : _settings) text += pair.first + ": " + pair.second + "\n";
    return text;
}

std::string Settings::getString(const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_settings.find(key) != _settings.end()) {
        return _settings[key];
    }
    return def;
}

int Settings::getInt(const std::string& key, int def) {
    std::string val = getString(key, "");
    if (val.empty()) return def;
    try {
        return std::stoi(val);
    } catch (...) {
        return def;
    }
}

bool Settings::getBool(const std::string& key, bool def) {
    std::string val = getString(key, "");
    if (val.empty()) return def;
    // Simple bool parsing
    if (val == "true" || val == "1" || val == "yes") return true;
    if (val == "false" || val == "0" || val == "no") return false;
    return def;
}

float Settings::getFloat(const std::string& key, float def) {
    std::string val = getString(key, "");
    if (val.empty()) return def;
    try {
        return std::stof(val);
    } catch (...) {
        return def;
    }
}

void Settings::set_text(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _settings.find(key);
        if (it != _settings.end() && it->second == value) return;   // nothing to write
        _settings[key] = value;
        mark_dirty();
    }
}

void Settings::remove_key(const std::string& key) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_settings.erase(key)) return;
        mark_dirty();
    }
}

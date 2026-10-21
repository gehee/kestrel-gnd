#include "settings.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <pthread.h>
#include <sched.h>

Settings& Settings::getInstance() {
    static Settings instance;
    return instance;
}

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
    if (_gen == _saved_gen) _due = std::chrono::steady_clock::now() + kSaveDelay;
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
        if (_gen == _saved_gen) {
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

    // Rewritten in place, as before: the file may be a link into a writable
    // partition, and a rename would replace the link rather than its target.
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open settings file for writing: " << filename << std::endl;
    } else {
        for (const auto& pair : snap) {
            file << pair.first << ": " << pair.second << "\n";
        }
    }

    // Marked done even when the open failed: a read-only file stays read-only,
    // and retrying it on every change would only repeat the message.
    std::lock_guard<std::mutex> lock(_mutex);
    if (gen > _saved_gen) _saved_gen = gen;
}

void Settings::flush() {
    write_snapshot();
}

void Settings::save() {
    flush();
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

void Settings::set(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _settings.find(key);
        if (it != _settings.end() && it->second == value) return;   // nothing to write
        _settings[key] = value;
        mark_dirty();
    }
}

void Settings::remove(const std::string& key) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_settings.erase(key)) return;
        mark_dirty();
    }
}

void Settings::set(const std::string& key, int value) {
    set(key, std::to_string(value));
}

void Settings::set(const std::string& key, bool value) {
    set(key, value ? "true" : "false");
}

void Settings::set(const std::string& key, float value) {
    set(key, std::to_string(value));
}

void Settings::set(const std::string& key, const char* value) {
    if (value) set(key, std::string(value));
    else set(key, std::string(""));
}

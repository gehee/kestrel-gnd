#include "settings.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

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

void Settings::save() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_filename.empty()) {
        return;
    }

    std::ofstream file(_filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open settings file for writing: " << _filename << std::endl;
        return;
    }

    for (const auto& pair : _settings) {
        file << pair.first << ": " << pair.second << "\n";
    }
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
        _settings[key] = value;
    }
    save();
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

#pragma once

#include <string>
#include <map>
#include <mutex>
#include <vector>

class Settings {
public:
    static Settings& getInstance();

    void load(const std::string& filename);
    void save();
    
    // Getters
    std::string getString(const std::string& key, const std::string& def);
    int getInt(const std::string& key, int def);
    bool getBool(const std::string& key, bool def);
    float getFloat(const std::string& key, float def);

    // Setters (automatically saves if changed)
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, const char* value); // Added overload to prevent bool conversion
    void set(const std::string& key, int value);
    void set(const std::string& key, bool value);
    void set(const std::string& key, float value);
    void remove(const std::string& key);   // saves, like set()

private:
    Settings() {}
    
    // Helper to trim whitespace
    static std::string trim(const std::string& str);

    std::map<std::string, std::string> _settings;
    std::string _filename;
    std::mutex _mutex;
};

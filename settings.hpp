#pragma once

#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <cstdint>

class Settings {
public:
    static Settings& getInstance();

    void load(const std::string& filename);
    void save();    // the same as flush()
    // Writes the file now if a change is not on disk yet. set() only queues the
    // write, so anything about to restart the app, or hand over to a script
    // or process that reads the file, calls this first.
    void flush();
    
    // Getters
    std::string getString(const std::string& key, const std::string& def);
    int getInt(const std::string& key, int def);
    bool getBool(const std::string& key, bool def);
    float getFloat(const std::string& key, float def);

    // Setters. The value is live at once; the file follows from a background
    // writer within ~200 ms (a burst of changes is one write), so a caller on
    // a real-time thread never waits on the filesystem.
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, const char* value); // Added overload to prevent bool conversion
    void set(const std::string& key, int value);
    void set(const std::string& key, bool value);
    void set(const std::string& key, float value);
    void remove(const std::string& key);   // saves, like set()

private:
    Settings() {}
    ~Settings();
    void mark_dirty();      // with _mutex held
    void saver_run();
    void write_snapshot();  // takes _write_mutex
    
    // Helper to trim whitespace
    static std::string trim(const std::string& str);

    std::map<std::string, std::string> _settings;
    std::string _filename;
    std::mutex _mutex;

    // Background writer. _gen counts changes, _saved_gen is the last one on
    // disk; _due is when the pending write goes. _write_mutex keeps the saver
    // and flush() from writing at once, so an older snapshot can never land
    // after a newer one. The thread starts with the first change.
    uint64_t _gen = 0, _saved_gen = 0;
    std::chrono::steady_clock::time_point _due;
    bool _saver_stop = false;
    std::thread _saver;
    std::condition_variable _saver_cv;
    std::mutex _write_mutex;
};

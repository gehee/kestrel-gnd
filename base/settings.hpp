#pragma once

#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <cstdint>

// One setting: its key in the settings file, its type and its default. Each
// part declares its own, once, in its settings header (<part>/settings.hpp),
// as a constexpr - no code, nothing made at start; together they are every
// key the file can hold, and a part can name only its own and those of the
// parts it is built on. A text setting's default is a C string there.
template <typename T> struct SettingDefault { using type = T; };
template <> struct SettingDefault<std::string> { using type = const char*; };
template <typename T> struct SettingValue { using type = T; };   // set()'s, not deduced from it
template <typename T>
struct Setting {
    const char* key;
    typename SettingDefault<T>::type def;
};
// A key with a name made at run time (one per screen, per air unit, per
// video mode): a function in the settings header makes it.
template <typename T>
struct MadeSetting {
    std::string key;
    T def;
};

// The settings file. main() makes the one there is and hands it to each part
// that keeps settings; nothing reaches for it.
class Settings {
public:
    Settings() {}
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    void load(const std::string& filename, bool atomic = false);
    // Writes the file now if a change is not on disk yet. set() only queues the
    // write, so anything about to restart the app, or hand over to a script
    // or process that reads the file, calls this first.
    void flush();

    // Each takes a Setting or a MadeSetting.
    template <template <typename> class S, typename T>
    T get(const S<T>& s) { return read(s.key, T(s.def)); }

    // The value is live at once; the file follows from a background writer
    // within ~200 ms (a burst of changes is one write), so a caller on a
    // real-time thread never waits on the filesystem.
    template <template <typename> class S, typename T>
    void set(const S<T>& s, const typename SettingValue<T>::type& v) { set_text(s.key, write(v)); }
    template <template <typename> class S, typename T>
    void remove(const S<T>& s) { remove_key(s.key); }   // saves, like set()

    // The value as the file holds it, "" when it holds none: for a setting
    // read in an older form of its own once (hud_reactivity), or written in a
    // form of its own (a period to the thousandth of a microsecond).
    template <template <typename> class S, typename T>
    std::string text(const S<T>& s) { return getString(s.key, ""); }
    template <template <typename> class S, typename T>
    void set_text(const S<T>& s, const std::string& v) { set_text(s.key, v); }

private:
    int read(const std::string& key, int def) { return getInt(key, def); }
    bool read(const std::string& key, bool def) { return getBool(key, def); }
    float read(const std::string& key, float def) { return getFloat(key, def); }
    std::string read(const std::string& key, const std::string& def) { return getString(key, def); }
    static std::string write(int v) { return std::to_string(v); }
    static std::string write(bool v) { return v ? "true" : "false"; }
    static std::string write(float v) { return std::to_string(v); }
    static std::string write(const std::string& v) { return v; }

    std::string getString(const std::string& key, const std::string& def);
    int getInt(const std::string& key, int def);
    bool getBool(const std::string& key, bool def);
    float getFloat(const std::string& key, float def);
    void set_text(const std::string& key, const std::string& value);
    void remove_key(const std::string& key);

    void mark_dirty();      // with _mutex held
    void saver_run();
    void write_snapshot();  // takes _write_mutex

    // Helper to trim whitespace
    static std::string trim(const std::string& str);

    std::map<std::string, std::string> _settings;
    std::string _filename;
    bool _atomic = false;
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

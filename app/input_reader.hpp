#ifndef INPUT_READER_HPP
#define INPUT_READER_HPP

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <iostream>
#include <cstring>
#include <sys/ioctl.h>

#ifndef NLONGS
#define NLONGS(x) (((x) + 8 * sizeof (unsigned long) - 1) / (8 * sizeof (unsigned long)))
#endif

class InputReader {
public:
    struct KeyEvent {
        int code;
        bool pressed;
    };

    InputReader() {
        // Try to find a keyboard device
        find_keyboards();
    }

    ~InputReader() {
        for (int fd : fds) {
            close(fd);
        }
    }

    std::vector<KeyEvent> read_events() {
        std::vector<KeyEvent> events;
        for (int fd : fds) {
            struct input_event ev;
            while (read(fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY) {
                    events.push_back({ev.code, ev.value > 0});
                }
            }
        }
        return events;
    }

    int get_poll_fd_count() const { return fds.size(); }
    const std::vector<int>& get_fds() const { return fds; }

    // Pick up devices created AFTER startup (e.g. the SSH menu bridge's uinput
    // keyboard) — the constructor scan alone would never see them. Incremental:
    // already-open paths are skipped.
    void rescan() { find_keyboards(); }

    // Drop a device whose node went away (bridge exited, keyboard unplugged);
    // without this the poll loop spins on POLLERR forever.
    void remove(int fd) {
        close(fd);
        fd_paths.erase(fd);
        fds.erase(std::remove(fds.begin(), fds.end(), fd), fds.end());
    }

private:
    std::vector<int> fds;
    std::map<int, std::string> fd_paths;   // fd -> /dev/input/eventN (open device nodes)

    bool have_path(const std::string& path) const {
        for (const auto& kv : fd_paths)
            if (kv.second == path) return true;
        return false;
    }

    void find_keyboards() {
        const char* dirname = "/dev/input";
        DIR* dir = opendir(dirname);
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (std::string(entry->d_name).find("event") == 0) {
                std::string path = std::string(dirname) + "/" + entry->d_name;
                if (have_path(path)) continue;
                int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    // Check if it's a keyboard
                    unsigned long key_bits[NLONGS(KEY_CNT)];
                    memset(key_bits, 0, sizeof(key_bits));
                    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
                        // Crude check: if it has KEY_ENTER and (KEY_A or KEY_UP), it's likely a keyboard or joystick navigation device
                        bool has_enter = (key_bits[KEY_ENTER / (8 * sizeof(long))] & (1L << (KEY_ENTER % (8 * sizeof(long))))) != 0;
                        bool has_navigation = (key_bits[KEY_A / (8 * sizeof(long))] & (1L << (KEY_A % (8 * sizeof(long))))) != 0 ||
                                              (key_bits[KEY_UP / (8 * sizeof(long))] & (1L << (KEY_UP % (8 * sizeof(long))))) != 0;
                        if (has_enter && has_navigation) {
                            printf("Found keyboard/navigation device: %s\n", path.c_str());
                            fds.push_back(fd);
                            fd_paths[fd] = path;
                        } else {
                            close(fd);
                        }
                    } else {
                        close(fd);
                    }
                }
            }
        }
        closedir(dir);
    }
};

#endif

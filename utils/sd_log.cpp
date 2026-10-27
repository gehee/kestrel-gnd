#include "utils/sd_log.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sd_log {
namespace {

constexpr size_t kKeep = 30;              // logs kept on the card
constexpr size_t kMaxPending = 1 << 20;   // text not yet on the card, at most
constexpr int kSyncMs = 2000;

// Everything the reader and writer threads touch. They are detached and run
// until the process ends, so this is made once and never destroyed: as plain
// globals, exit() ran their destructors while the writer could still be
// swapping `pending` (a double free, SIGABRT in sd-log-write after
// "kestrel-gnd done."), and exit's own flush of stdout reaches the reader.
struct State {
    std::string log_dir;
    int out_fd = -1;     // where stdout went before (the service's log), once redirected
    std::mutex m;        // the three below
    std::condition_variable cv;
    int file_fd = -1;    // the log on the card, -1 while off
    std::string pending;
    size_t dropped = 0;
    std::mutex write_m;  // one writer on the card at a time (the thread, or turning off)
};
State& state = *new State();   // never deleted, on purpose

std::string& log_dir = state.log_dir;
int& out_fd = state.out_fd;
std::mutex& m = state.m;
std::condition_variable& cv = state.cv;
int& file_fd = state.file_fd;
std::string& pending = state.pending;
size_t& dropped = state.dropped;
std::mutex& write_m = state.write_m;

// Oldest first by the time in the name; the newest kKeep - 1 stay, the one
// about to be made is the kKeep-th.
void prune(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> logs;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind("fpvOS_", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
            logs.push_back(n);
    }
    closedir(d);
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + (kKeep - 1) < logs.size(); i++) unlink((dir + "/" + logs[i]).c_str());
}

void write_out(int fd, const std::string& chunk, size_t lost) {
    if (lost) {
        char note[96];
        const int k = snprintf(note, sizeof note, "sd-log: %zu bytes not written, the card was behind\n", lost);
        if (k > 0 && write(fd, note, (size_t)k) < 0) {}
    }
    for (size_t w = 0; w < chunk.size();) {
        const ssize_t r = write(fd, chunk.data() + w, chunk.size() - w);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        w += (size_t)r;
    }
    if (!chunk.empty() || lost) fsync(fd);
}

void reader(int pipe_fd) {
    pthread_setname_np(pthread_self(), "sd-log-read");
    char buf[4096];
    for (;;) {
        const ssize_t n = read(pipe_fd, buf, sizeof buf);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        for (ssize_t w = 0; w < n;) {                     // the service's log first, as before
            const ssize_t r = write(out_fd, buf + w, (size_t)(n - w));
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
            w += r;
        }
        std::lock_guard<std::mutex> lk(m);
        if (file_fd < 0) continue;                        // off
        if (pending.size() + (size_t)n <= kMaxPending) pending.append(buf, (size_t)n);
        else dropped += (size_t)n;
    }
}

void writer() {
    pthread_setname_np(pthread_self(), "sd-log-write");
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::milliseconds(kSyncMs));
        }
        std::lock_guard<std::mutex> wl(write_m);
        std::string chunk;
        size_t lost = 0;
        int fd;
        {
            std::lock_guard<std::mutex> lk(m);
            chunk.swap(pending);
            lost = dropped; dropped = 0;
            fd = file_fd;
        }
        if (fd >= 0) write_out(fd, chunk, lost);
    }
}

// stdout and stderr through a pipe: once, at the first log.
bool redirect() {
    if (out_fd >= 0) return true;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return false;
    fflush(stdout); fflush(stderr);
    const int fd = dup(STDOUT_FILENO);
    if (fd < 0 || dup2(p[1], STDOUT_FILENO) < 0 || dup2(p[1], STDERR_FILENO) < 0) {
        printf("sd-log: cannot redirect the output\n");
        return false;
    }
    out_fd = fd;
    close(p[1]);
    std::thread(reader, p[0]).detach();
    std::thread(writer).detach();
    return true;
}

}  // namespace

void init(const std::string& dir) { log_dir = dir; }

void set_enabled(bool on) {
    if (!on) {
        // What is pending goes to the card, then the log is closed.
        std::lock_guard<std::mutex> wl(write_m);
        int fd;
        std::string chunk;
        size_t lost;
        {
            std::lock_guard<std::mutex> lk(m);
            if (file_fd < 0) return;
            fd = file_fd; file_fd = -1;
            chunk.swap(pending);
            lost = dropped; dropped = 0;
        }
        static const char kOff[] = "sd-log: off (HUD > Debug Info)\n";
        chunk += kOff;
        write_out(fd, chunk, lost);
        close(fd);
        printf("%s", kOff);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m);
        if (file_fd >= 0 || log_dir.empty()) return;
    }
    // The partition, not the mount point S35dvrpart makes in any case: a
    // directory on the same device as its parent is not mounted (as the DVR
    // checks it).
    struct stat st, parent;
    if (stat(log_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        stat((log_dir + "/..").c_str(), &parent) != 0 || parent.st_dev == st.st_dev) {
        printf("sd-log: %s is not a mounted recordings partition - the log stays in RAM only\n", log_dir.c_str());
        return;
    }
    prune(log_dir);
    // Named as the recordings are (DVR::format_timestamped_filename: local
    // time, fpvOS_YYYYMMDD_HHMMSS), so a flight's log sorts with its videos.
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &lt);
    const std::string path = log_dir + "/fpvOS_" + stamp + ".log";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) { printf("sd-log: cannot open %s: %s\n", path.c_str(), strerror(errno)); return; }
    if (!redirect()) { close(fd); return; }
    {
        std::lock_guard<std::mutex> lk(m);
        file_fd = fd;
    }
    printf("sd-log: this log is also in %s (HUD > Debug Info is on)\n", path.c_str());
}

void restore_output() {
    if (out_fd < 0) return;
    dup2(out_fd, STDOUT_FILENO);
    dup2(out_fd, STDERR_FILENO);
}

}  // namespace sd_log

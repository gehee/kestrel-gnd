#include "base/sd_log.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sys/ioctl.h>
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
    // Where stdout went before (the service's log), once redirected; and the
    // pipe's end the reader reads, with whether it holds text it has read
    // and not passed on yet. Atomics: a signal handler reads them.
    std::atomic<int> out_fd{-1};
    std::atomic<int> pipe_fd{-1};
    std::atomic<bool> reader_busy{false};
    std::mutex m;        // the three below
    std::condition_variable cv;
    std::atomic<int> file_fd{-1};   // the log on the card, -1 while off (set under m)
    std::string path;    // ... and its name
    std::string pending;
    size_t dropped = 0;
    std::mutex write_m;  // one writer on the card at a time (the thread, or turning off)
};
State& state = *new State();   // never deleted, on purpose

std::string& log_dir = state.log_dir;
std::atomic<int>& out_fd = state.out_fd;
std::atomic<int>& pipe_fd = state.pipe_fd;
std::atomic<bool>& reader_busy = state.reader_busy;
std::mutex& m = state.m;
std::condition_variable& cv = state.cv;
std::atomic<int>& file_fd = state.file_fd;
std::string& pending = state.pending;
std::string& path_now = state.path;
size_t& dropped = state.dropped;
std::mutex& write_m = state.write_m;

// A log's number, from the line it starts with (0: a log from before they
// were numbered). The clock cannot tell the order the logs were made in: the
// PMIC's clock comes up at 2017-08-04 09:00 on every cold boot the pcf8563
// is not read (S36rtc), and then names and mtimes alike call a new log older
// than last year's.
unsigned log_number(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char head[64];
    const ssize_t n = read(fd, head, sizeof head - 1);
    close(fd);
    if (n <= 0) return 0;
    head[n] = 0;
    unsigned v = 0;
    return sscanf(head, "sd-log: log #%u", &v) == 1 ? v : 0;
}

// Oldest first by number (unnumbered ones first, by name); the newest
// kKeep - 1 stay, the one about to be made is the kKeep-th. Its number.
unsigned prune(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return 1;
    std::vector<std::pair<unsigned, std::string>> logs;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind("fpvOS_", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
            logs.emplace_back(log_number(dir + "/" + n), n);
    }
    closedir(d);
    std::sort(logs.begin(), logs.end());
    const unsigned next = logs.empty() ? 1 : logs.back().first + 1;
    for (size_t i = 0; i + (kKeep - 1) < logs.size(); i++) {
        const std::string& n = logs[i].second;
        unlink((dir + "/" + n).c_str());
        // its per-frame radio log with it
        unlink((dir + "/" + n.substr(0, n.size() - 4) + ".frames.csv").c_str());
    }
    return next;
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
        reader_busy = true;
        for (ssize_t w = 0; w < n;) {                     // the service's log first, as before
            const ssize_t r = write(out_fd, buf + w, (size_t)(n - w));
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
            w += r;
        }
        std::lock_guard<std::mutex> lk(m);
        if (file_fd >= 0) {                               // else off
            if (pending.size() + (size_t)n <= kMaxPending) pending.append(buf, (size_t)n);
            else dropped += (size_t)n;
        }
        reader_busy = false;
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
    const int fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);   // not for children
    if (fd < 0 || dup2(p[1], STDOUT_FILENO) < 0 || dup2(p[1], STDERR_FILENO) < 0) {
        printf("sd-log: cannot redirect the output\n");
        return false;
    }
    out_fd = fd;
    close(p[1]);
    pipe_fd = p[0];
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
    const unsigned number = prune(log_dir);
    // Named as the recordings are (DVR::format_timestamped_filename: local
    // time, fpvOS_YYYYMMDD_HHMMSS), so a flight's log sorts with its videos.
    // A name already there gets _1, _2...: with the clock reset (see
    // log_number), two boots reach this at the same second.
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &lt);
    std::string path;
    int fd = -1;
    for (int k = 0; k < 100 && fd < 0; k++) {
        path = log_dir + "/fpvOS_" + stamp + (k ? "_" + std::to_string(k) : "") + ".log";
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) { printf("sd-log: cannot open %s: %s\n", path.c_str(), strerror(errno)); return; }
    {
        char head[64];
        const int k = snprintf(head, sizeof head, "sd-log: log #%u on this card\n", number);
        if (k > 0 && write(fd, head, (size_t)k) < 0) {}
    }
    if (!redirect()) { close(fd); return; }
    {
        std::lock_guard<std::mutex> lk(m);
        file_fd = fd;
        path_now = path;
    }
    printf("sd-log: this log is also in %s (HUD > Debug Info is on)\n", path.c_str());
}

std::string current_path() {
    std::lock_guard<std::mutex> lk(m);
    return file_fd >= 0 ? path_now : std::string();
}

void restore_output() {
    const int fd = out_fd;
    if (fd < 0) return;
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
}

namespace {
void sleep_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

// Until the reader has passed on all that is in the pipe, or ms are up.
// Idle twice in a row: it may have read a chunk and not yet said so.
void wait_drained(int fd, int ms) {
    bool was_idle = false;
    for (int t = 0; t < ms; t += 5) {
        int queued = 0;
        const bool idle = ioctl(fd, FIONREAD, &queued) == 0 && queued == 0 && !reader_busy;
        if (idle && was_idle) return;
        was_idle = idle;
        sleep_ms(5);
    }
}
}  // namespace

void flush(bool may_wait) {
    const int rd = pipe_fd;
    if (rd < 0) return;   // never on
    if (may_wait) { fflush(stdout); fflush(stderr); }
    wait_drained(rd, may_wait ? 300 : 100);
    std::unique_lock<std::mutex> wl(write_m, std::defer_lock);
    if (may_wait) {
        wl.lock();
    } else {
        // The writer holds it only while it writes a chunk: a moment, unless
        // it is the thread that crashed.
        for (int t = 0; t < 100 && !wl.try_lock(); t += 5) sleep_ms(5);
        if (!wl.owns_lock()) return;
    }
    std::unique_lock<std::mutex> lk(m, std::defer_lock);
    if (may_wait) lk.lock();
    else if (!lk.try_lock()) return;
    const int fd = file_fd;
    if (fd < 0) return;
    if (may_wait) {
        std::string chunk;
        chunk.swap(pending);
        const size_t lost = dropped;
        dropped = 0;
        lk.unlock();
        write_out(fd, chunk, lost);
        return;
    }
    // No allocation here: the text as it is, then nothing pending.
    for (size_t w = 0; w < pending.size();) {
        const ssize_t r = write(fd, pending.data() + w, pending.size() - w);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        w += (size_t)r;
    }
    pending.clear();
    fsync(fd);
}

void crash_fds(int* out, int* card) {
    const int o = out_fd;
    *out = o >= 0 ? o : STDERR_FILENO;
    *card = file_fd;
}

}  // namespace sd_log

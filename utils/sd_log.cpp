#include "sd_log.hpp"

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

int out_fd = -1;     // where stdout went before (the service's log)
int file_fd = -1;    // the log on the card
std::mutex m;
std::condition_variable cv;
std::string pending;
size_t dropped = 0;

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
        if (pending.size() + (size_t)n <= kMaxPending) pending.append(buf, (size_t)n);
        else dropped += (size_t)n;
    }
}

void writer() {
    pthread_setname_np(pthread_self(), "sd-log-write");
    for (;;) {
        std::string chunk;
        size_t lost = 0;
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::milliseconds(kSyncMs));
            chunk.swap(pending);
            lost = dropped; dropped = 0;
        }
        if (lost) {
            char note[96];
            const int k = snprintf(note, sizeof note, "sd-log: %zu bytes not written, the card was behind\n", lost);
            if (k > 0 && write(file_fd, note, (size_t)k) < 0) {}
        }
        for (size_t w = 0; w < chunk.size();) {
            const ssize_t r = write(file_fd, chunk.data() + w, chunk.size() - w);
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
            w += (size_t)r;
        }
        if (!chunk.empty() || lost) fsync(file_fd);
    }
}

}  // namespace

void start(const std::string& dir) {
    // The partition, not the mount point S35dvrpart makes in any case: a
    // directory on the same device as its parent is not mounted (as the DVR
    // checks it).
    struct stat st, parent;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        stat((dir + "/..").c_str(), &parent) != 0 || parent.st_dev == st.st_dev) {
        printf("sd-log: %s is not a mounted recordings partition - the log stays in RAM only\n", dir.c_str());
        return;
    }
    prune(dir);
    // Named as the recordings are (DVR::format_timestamped_filename: local
    // time, fpvOS_YYYYMMDD_HHMMSS), so a flight's log sorts with its videos.
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &lt);
    const std::string path = dir + "/fpvOS_" + stamp + ".log";
    file_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (file_fd < 0) { printf("sd-log: cannot open %s: %s\n", path.c_str(), strerror(errno)); return; }
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) { close(file_fd); file_fd = -1; return; }
    fflush(stdout); fflush(stderr);
    out_fd = dup(STDOUT_FILENO);
    if (out_fd < 0 || dup2(p[1], STDOUT_FILENO) < 0 || dup2(p[1], STDERR_FILENO) < 0) {
        printf("sd-log: cannot redirect the output\n");
        return;
    }
    close(p[1]);
    std::thread(reader, p[0]).detach();
    std::thread(writer).detach();
    printf("sd-log: this log is also in %s\n", path.c_str());
}

void restore_output() {
    if (out_fd < 0) return;
    dup2(out_fd, STDOUT_FILENO);
    dup2(out_fd, STDERR_FILENO);
}

}  // namespace sd_log

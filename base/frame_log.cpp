#include "base/frame_log.hpp"
#include <condition_variable>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace frame_log {
namespace {
struct State {
    std::mutex m;
    std::condition_variable cv;
    std::string path, header, pending;
    size_t dropped = 0;
    bool started = false;
};
State& st = *new State();   // never deleted, on purpose (threads outlive main's statics)

void run() {
    std::string open_path;
    int fd = -1;
    for (;;) {
        std::string path, header, chunk;
        size_t lost;
        {
            std::unique_lock<std::mutex> lk(st.m);
            st.cv.wait_for(lk, std::chrono::seconds(2));
            path = st.path; header = st.header; chunk.swap(st.pending); lost = st.dropped; st.dropped = 0;
        }
        if (path != open_path) {
            if (fd >= 0) { fsync(fd); close(fd); }
            fd = path.empty() ? -1 : open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            open_path = path;
            if (fd >= 0 && lseek(fd, 0, SEEK_END) == 0) chunk = header + chunk;
        }
        if (fd < 0) continue;
        if (lost) {
            char note[64];
            const int k = snprintf(note, sizeof note, "# %zu bytes lost, the card was behind\n", lost);
            if (k > 0 && write(fd, note, (size_t)k) < 0) {}
        }
        for (size_t w = 0; w < chunk.size();) {
            const ssize_t r = write(fd, chunk.data() + w, chunk.size() - w);
            if (r <= 0) break;
            w += (size_t)r;
        }
        fsync(fd);
    }
}
}  // namespace

void append(const std::string& path, const std::string& header, std::string lines) {
    std::lock_guard<std::mutex> lk(st.m);
    if (!st.started) { st.started = true; std::thread(run).detach(); }
    if (path != st.path) { st.path = path; st.header = header; }
    if (st.pending.size() + lines.size() > (1u << 20)) { st.dropped += lines.size(); return; }
    st.pending += lines;
}
}  // namespace frame_log

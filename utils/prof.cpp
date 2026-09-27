#include "prof.hpp"
#include "time_util.h"

#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace prof {
namespace {

const bool s_on = getenv("KESTREL_PROF") != nullptr;
constexpr uint64_t kWindowUs = 5000000;

const char* const kSecName[kSecCount]   = { "snapshot", "pre", "canopy", "bf", "rest", "swap", "lock", "handoff" };
const char* const kWakeName[kWakeCount] = { "video", "anim", "input", "link", "timeout", "other" };
const char* const kCountName[kCountN]   = { "osd-commit", "osd-pending", "flip-busy", "text-miss" };

std::atomic<unsigned> s_wake[kWakeCount];
std::atomic<unsigned> s_count[kCountN];

// OSD thread only.
uint64_t s_window_start = 0, s_last = 0, s_frame_start = 0;
unsigned s_frames = 0;
std::vector<float> s_sec[kSecCount];
std::vector<float> s_total, s_gpu;
struct Pending { int fd; uint64_t submit_us; };
std::deque<Pending> s_fences;

float pct(std::vector<float>& v, float p) {
    if (v.empty()) return 0.0f;
    size_t k = std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}
float mean(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    double s = 0; for (float x : v) s += x;
    return (float)(s / v.size());
}

// Collect fences the GPU has signalled; the kernel stamps the signal time.
void reap_fences() {
    while (!s_fences.empty()) {
        Pending p = s_fences.front();
        struct pollfd pfd = { p.fd, POLLIN, 0 };
        if (poll(&pfd, 1, 0) <= 0) break;          // oldest not done: later ones won't be either
        struct sync_fence_info fi = {};
        struct sync_file_info info = {};
        info.num_fences = 1;
        info.sync_fence_info = (uint64_t)(uintptr_t)&fi;
        if (ioctl(p.fd, SYNC_IOC_FILE_INFO, &info) == 0 && fi.timestamp_ns) {
            int64_t d = (int64_t)(fi.timestamp_ns / 1000) - (int64_t)p.submit_us;
            if (d >= 0 && d < 1000000) s_gpu.push_back(d / 1000.0f);
        }
        close(p.fd);
        s_fences.pop_front();
    }
    while (s_fences.size() > 64) { close(s_fences.front().fd); s_fences.pop_front(); }
}

void report(uint64_t now) {
    double secs = (now - s_window_start) / 1e6;
    printf("PROF %.1f renders/s | frame mean %.2f p99 %.2f ms |", s_frames / secs, mean(s_total), pct(s_total, 0.99f));
    for (int i = 0; i < kSecCount; i++)
        printf(" %s %.2f/%.2f", kSecName[i], mean(s_sec[i]), pct(s_sec[i], 0.99f));
    printf(" | gpu %.2f/%.2f ms (%zu)", mean(s_gpu), pct(s_gpu, 0.99f), s_gpu.size());
    printf(" | wakes/s");
    for (int i = 0; i < kWakeCount; i++) printf(" %s %.0f", kWakeName[i], s_wake[i].exchange(0) / secs);
    printf(" | /s");
    for (int i = 0; i < kCountN; i++) printf(" %s %.0f", kCountName[i], s_count[i].exchange(0) / secs);
    printf("\n");
    fflush(stdout);
    for (auto& v : s_sec) v.clear();
    s_total.clear(); s_gpu.clear();
    s_frames = 0;
    s_window_start = now;
}

}  // namespace

bool enabled() { return s_on; }

void frame_begin() {
    if (!s_on) return;
    reap_fences();
    s_frame_start = s_last = get_time_us();
    if (!s_window_start) s_window_start = s_frame_start;
}

void mark(Sec s) {
    if (!s_on) return;
    uint64_t t = get_time_us();
    s_sec[s].push_back((t - s_last) / 1000.0f);
    s_last = t;
}

void frame_end() {
    if (!s_on) return;
    uint64_t t = get_time_us();
    s_total.push_back((t - s_frame_start) / 1000.0f);
    s_frames++;
    if (t - s_window_start >= kWindowUs) report(t);
}

void wake(Wake w)  { if (s_on) s_wake[w]++; }
void count(int id) { if (s_on && id >= 0 && id < kCountN) s_count[id]++; }

void gpu_fence(int fd, uint64_t submit_us) {
    if (fd < 0) return;
    if (!s_on) { close(fd); return; }
    s_fences.push_back({fd, submit_us});
}

}  // namespace prof

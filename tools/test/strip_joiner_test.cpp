// Unit test of the strip joiner (hal/rockchip/rga_strips.cpp) with RGA and
// the DRM device stubbed: strips of pictures fed as the strips' decoders do,
// and what comes out - whole pictures, at the size the strips add up to, the
// renderer flushed before any buffer it may hold is made again.
//   - a new strip count (OFF/2/3/4 in the menu) needs no new buffers: the
//     picture stays 1080 rows, one picture is given up at the change;
//   - a new size (another video mode) makes the buffers again once, flushing
//     the renderer and the screen recording's pictures first, never sized
//     from a mix of the old strips' heights and the new ones';
//   - the strips' decoder threads at once, with the renderer slow to let go,
//     while the size changes: every copy within its buffer, pictures coming
//     out at the new size after it (the race that froze the picture: the
//     buffers freed under a size already set, every join failing after).
//   cmake -DKESTREL_TESTS=ON ... && ctest
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <xf86drm.h>

#include "hal/rockchip/rga_strips.hpp"
#include "include/rga/im2d.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// ---- RGA and DRM, stubbed ----
// Each buffer made has an RGA handle that knows its height and whether it was
// let go: a copy into one let go, or past its rows, is a failure.
static std::mutex g_m;
static std::set<rga_buffer_handle_t> g_live;
static std::atomic<int> g_handles{0}, g_copies{0}, g_bad{0};
rga_buffer_handle_t importbuffer_fd(int, int) {
    std::lock_guard<std::mutex> lk(g_m);
    const rga_buffer_handle_t h = ++g_handles;
    g_live.insert(h);
    return h;
}
IM_STATUS releasebuffer_handle(rga_buffer_handle_t h) {
    std::lock_guard<std::mutex> lk(g_m);
    g_live.erase(h);
    return IM_STATUS_SUCCESS;
}
rga_buffer_t wrapbuffer_handle(rga_buffer_handle_t h, int w, int ht, int f, int, int) {
    rga_buffer_t b = {};
    b.handle = h;
    b.width = w;
    b.height = ht;
    b.format = f;
    return b;
}
IM_STATUS improcess(rga_buffer_t src, rga_buffer_t dst, rga_buffer_t, im_rect s, im_rect d, im_rect, int) {
    g_copies++;
    bool live;
    {
        std::lock_guard<std::mutex> lk(g_m);
        live = g_live.count(dst.handle) && g_live.count(src.handle);
    }
    if (!dst.handle || !live || d.y < 0 || d.y + d.height > dst.height || s.y + s.height > src.height) {
        if (++g_bad <= 5) std::printf("  bad copy: into %d (%s) rows %d+%d of %d\n", dst.handle,
                                      live ? "live" : "let go", d.y, d.height, dst.height);
        return IM_STATUS_FAILED;
    }
    return IM_STATUS_SUCCESS;
}
extern "C" const char* querystring(int) { return "stub"; }
extern "C" const char* imStrError_t(IM_STATUS) { return "stub"; }
extern "C" int ioctl(int, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (req == DRM_IOCTL_MODE_CREATE_DUMB) {
        auto* c = (drm_mode_create_dumb*)arg;
        c->pitch = c->width;
        c->handle = 1;
        return 0;
    }
    if (req == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
        ((drm_prime_handle*)arg)->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        return 0;
    }
    return 0;
}

// ---- the renderer and the screen ----
struct Sink : hal::FrameSink {
    std::atomic<int> frames{0}, flushes{0}, wrong{0};
    std::atomic<uint32_t> h{0}, want{0};
    std::atomic<uint32_t> epoch{0};
    std::atomic<int> epochs{0};
    int slow_us = 0;
    void queue_frame(std::shared_ptr<DecodedUnit> du) override {
        frames++;
        h = du->height;
        if (want && du->height != want) wrong++;
        if (du->buf_epoch != epoch.exchange(du->buf_epoch)) epochs++;
    }
    void request_flush() override {
        flushes++;
        if (slow_us) usleep(slow_us);
    }
};
struct Scan : hal::Scanout {
    std::atomic<int> drops{0};
    int scanout_fd() const override { return 3; }
    bool screen_recording() const override { return false; }
    void drop_screen_pictures() override { drops++; }
};

static std::shared_ptr<DecodedUnit> strip_pic(int64_t pic, uint32_t h, int fd) {
    auto d = std::make_shared<DecodedUnit>();
    d->pts = (uint64_t)pic;
    d->width = 1920;
    d->height = h;
    d->has_prime_fd = true;
    d->prime_fd = fd;
    d->pitches[0] = 1920;
    d->offsets[1] = 1920 * h;
    d->buf_epoch = 1;
    return d;
}

// The air unit's strips at 1080 rows (16 past each join): 2, 3 and 4 of them.
static const uint32_t k2[2] = {592, 520}, k3[3] = {416, 368, 360}, k4[4] = {336, 288, 288, 264};
// Two strips of an 864-row picture.
static const uint32_t k2_864[2] = {448, 448};

struct Feed {
    RgaStripJoiner& j;
    Sink& sink;
    int64_t pic = 100;
    // pictures of n strips, each strip in order; how many came out whole.
    int run(int n, const uint32_t* hs, int pics) {
        const int f0 = sink.frames;
        for (int p = 0; p < pics; p++, pic++)
            for (int k = 0; k < n; k++) j.strip(k, k == n - 1, 16, strip_pic(pic, hs[k], 10 + k));
        return sink.frames - f0;
    }
};

static void test_counts_and_sizes() {
    auto sink = std::make_shared<Sink>();
    auto scan = std::make_shared<Scan>();
    RgaStripJoiner j(sink, scan);
    Feed f{j, *sink};
    // The first picture of all: its count not known till its last strip.
    CHECK(f.run(4, k4, 30) >= 29);
    CHECK(sink->h == 1080);
    const int flushes = sink->flushes;
    const int epochs = sink->epochs;
    // A new count: same size, the same buffers, no flush, one picture lost.
    sink->want = 1080;
    CHECK(f.run(2, k2, 30) == 29);
    CHECK(f.run(3, k3, 30) == 29);
    CHECK(f.run(4, k4, 30) == 29);
    CHECK(f.run(2, k2, 30) == 29);
    CHECK(sink->flushes == flushes);
    CHECK(sink->epochs == epochs);
    CHECK(sink->wrong == 0);
    // A new size: the buffers made once, after the renderer and the screen
    // recording let go; the pictures out of them a new epoch.
    sink->want = 864;
    const int drops = scan->drops;
    const int sized = f.run(2, k2_864, 30);
    std::printf("new size: %d of 30 pictures out\n", sized);
    CHECK(sized >= 29);
    CHECK(sink->h == 864);
    CHECK(sink->flushes == flushes + 1);
    CHECK(scan->drops == drops + 1);
    CHECK(sink->epochs == epochs + 1);
    CHECK(sink->wrong == 0);
    // ...and back, with a new count at the same time.
    sink->want = 1080;
    CHECK(f.run(3, k3, 30) == 29);
    CHECK(sink->flushes == flushes + 2);
    CHECK(sink->wrong == 0);
}

static void test_missing_and_late() {
    auto sink = std::make_shared<Sink>();
    auto scan = std::make_shared<Scan>();
    RgaStripJoiner j(sink, scan);
    Feed f{j, *sink};
    f.run(2, k2, 10);
    // A picture without its bottom strip is never shown; the next one is.
    int out = sink->frames;
    j.strip(0, false, 16, strip_pic(f.pic, k2[0], 10));
    f.pic++;
    f.run(2, k2, 1);
    CHECK(sink->frames == out + 1);
    // The bottom strip decoded before the top one: still one whole picture.
    out = sink->frames;
    j.strip(1, true, 16, strip_pic(f.pic, k2[1], 11));
    j.strip(0, false, 16, strip_pic(f.pic, k2[0], 10));
    f.pic++;
    CHECK(sink->frames == out + 1);
    // A strip of a picture already given up on: nothing.
    out = sink->frames;
    j.strip(1, true, 16, strip_pic(f.pic - 5, k2[1], 11));
    CHECK(sink->frames == out);
    // The air unit counting from 0 again (its kestrel-air restarted).
    f.pic = 3;
    CHECK(f.run(2, k2, 10) >= 9);
    CHECK(sink->h == 1080);
}

// Each strip on a thread of its own, as each has its own decoder, with the
// renderer taking 5 ms to let go; the size changes back and forth meanwhile.
static void test_threads_and_resize() {
    auto sink = std::make_shared<Sink>();
    auto scan = std::make_shared<Scan>();
    sink->slow_us = 5000;
    RgaStripJoiner j(sink, scan);
    const int kPics = 600;
    std::atomic<int> turn[2] = {{0}, {0}};
    auto worker = [&](int k) {
        for (int p = 0; p < kPics; p++) {
            const bool small = (p / 50) % 2;   // 50 pictures at 1080, 50 at 864, ...
            const uint32_t h = small ? k2_864[k] : k2[k];
            j.strip(k, k == 1, 16, strip_pic(1000 + p, h, 10 + k));
            turn[k] = p;
            // Neither strip more than a picture ahead of the other.
            while (turn[1 - k] < p - 1) std::this_thread::yield();
        }
    };
    const int c0 = g_bad;
    std::thread a(worker, 0), b(worker, 1);
    a.join();
    b.join();
    CHECK(g_bad == c0);
    // Pictures keep being joined across the resizes; how many depends on the
    // machine's load (a strip ahead of a 5 ms flush gives its picture up).
    CHECK(sink->frames > kPics / 10);
    CHECK(sink->h == 864);
    // Afterwards, on the size it settled on, every picture joins.
    Feed f{j, *sink};
    f.pic = 1000 + kPics;
    sink->want = 864;
    CHECK(f.run(2, k2_864, 20) == 20);
    CHECK(sink->wrong == 0);
    std::printf("threads: %d of %d pictures joined across %d resizes, %d flushes\n", (int)sink->frames, kPics,
                kPics / 50 - 1, (int)sink->flushes);
}

int main() {
    test_counts_and_sizes();
    test_missing_and_late();
    test_threads_and_resize();
    CHECK(g_bad == 0);
    if (fails) {
        std::printf("strip_joiner_test: %d FAILED (%d copies, %d bad)\n", fails, (int)g_copies, (int)g_bad);
        return 1;
    }
    std::printf("strip_joiner_test: ok (%d copies)\n", (int)g_copies);
    return 0;
}

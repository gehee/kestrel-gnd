// On-device DMA-BUF composition check; does not acquire DRM master or alter HDMI.
#include "hal/artosyn/ge2d_compositor.hpp"
#include <drm.h>
#include <drm_mode.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

static void check(bool ok, const char *what) {
    if (!ok) throw std::system_error(errno, std::generic_category(), what);
}
struct Buffer {
    int drm = -1, fd = -1;
    uint32_t handle = 0, stride = 0;
    size_t bytes = 0;
    uint8_t *data = nullptr;
    Buffer(unsigned width, unsigned height, unsigned bpp) {
        try {
            drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
            check(drm >= 0, "open DRM");
            drm_mode_create_dumb create{};
            create.width = width; create.height = height; create.bpp = bpp;
            check(ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0, "allocate DMA surface");
            handle = create.handle; stride = create.pitch; bytes = create.size;
            drm_prime_handle prime{}; prime.handle = handle; prime.flags = DRM_CLOEXEC | DRM_RDWR;
            check(ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0, "export DMA surface");
            fd = prime.fd;
            drm_mode_map_dumb map{}; map.handle = handle;
            check(ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "map DMA surface");
            void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, drm, map.offset);
            check(p != MAP_FAILED, "mmap DMA surface"); data = static_cast<uint8_t *>(p);
        } catch (...) { release(); throw; }
    }
    ~Buffer() { release(); }
    void release() {
        if (data) munmap(data, bytes);
        if (fd >= 0) close(fd);
        if (handle) { drm_mode_destroy_dumb d{}; d.handle = handle; ioctl(drm, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
        if (drm >= 0) close(drm);
    }
    void sync(uint64_t flags) { dma_buf_sync s{flags}; check(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0, "DMA CPU ownership"); }
    ar_ge2d_image yuv(unsigned width, unsigned height, unsigned coded_height) const {
        ar_ge2d_image v{}; v.width = width; v.height = height; v.format = AR_GE2D_YUV420;
        for (int i = 0; i < 3; ++i) { v.fd[i] = fd; v.stride[i] = stride / (i ? 2 : 1); }
        v.offset[1] = stride * coded_height; v.offset[2] = stride * coded_height * 5 / 4;
        return v;
    }
};
int main(int argc, char **argv) {
    try {
        const unsigned w = argc > 1 && std::string(argv[1]) == "1080" ? 1920 : 1280;
        const unsigned h = w == 1920 ? 1080 : 720, coded = (h + 15) & ~15u;
        hal::Ge2dCompositor compositor;
        Buffer source(w, coded * 3 / 2, 8), destination(w, coded * 3 / 2, 8), hud(320, 160, 32);
        auto src = source.yuv(w, h, coded), dst = destination.yuv(w, h, coded);
        source.sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        memset(source.data, 64, source.stride * coded);
        memset(source.data + src.offset[1], 128, source.stride * coded / 2);
        source.sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        hud.sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        for (unsigned y = 0; y < 160; ++y)
            for (unsigned x = 0; x < 320; ++x)
                reinterpret_cast<uint32_t *>(hud.data + y * hud.stride)[x] = 0x80ffffff;
        hud.sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        hal::Ge2dOverlay panel;
        panel.image.fd[0] = hud.fd; panel.image.width = 320; panel.image.height = 160;
        panel.image.format = AR_GE2D_ARGB8888; panel.image.stride[0] = hud.stride;
        for (const auto position : {std::pair<int,int>{64, 64}, {-13, -7}, {int(w)-9, int(h)-11}, {-400, 0}}) {
            panel.x = position.first; panel.y = position.second;
            destination.sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            memset(destination.data, 0xcd, destination.bytes);
            destination.sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            compositor.compose(src, dst, {panel});
            destination.sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
                bool inside = int(x) >= panel.x && int(y) >= panel.y && int(x) < panel.x + 320 && int(y) < panel.y + 160;
                int expected = inside ? 150 : 64;
                if (std::abs(int(destination.data[y * destination.stride + x]) - expected) > 2)
                    throw std::runtime_error("YUV/HUD pixel mismatch at " + std::to_string(x) + "," + std::to_string(y));
            }
            for (unsigned plane = 0; plane < 3; ++plane) {
                const unsigned rows = h / (plane ? 2 : 1), backing = coded / (plane ? 2 : 1);
                for (unsigned y = rows; y < backing; ++y)
                    for (unsigned x = 0; x < dst.stride[plane]; ++x)
                        if (destination.data[dst.offset[plane] + y * dst.stride[plane] + x] != 0xcd)
                            throw std::runtime_error("Composition overwrote padded rows");
            }
            for (unsigned plane = 1; plane < 3; ++plane)
                for (unsigned y = 0; y < h/2; ++y)
                    for (unsigned x = 0; x < w/2; ++x)
                        if (destination.data[dst.offset[plane] + y * dst.stride[plane] + x] != 128)
                            throw std::runtime_error("Neutral chroma changed during composition");
            destination.sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        }
        bool rejected = false;
        try { compositor.compose(src, src, {}); }
        catch (const std::system_error &e) { rejected = e.code().value() == EINVAL; }
        check(rejected, "reject output alias");
        auto invalid = src; invalid.offset[0] = 0xffffffe0;
        rejected = false;
        try { compositor.compose(invalid, dst, {}); }
        catch (const std::system_error &e) { rejected = e.code().value() == EINVAL; }
        check(rejected, "reject invalid DMA extent");
        panel.x = 64; panel.y = 64;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) compositor.compose(src, dst, {panel});
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        printf("PASS %ux%u DMA-BUF composition, clipping, guards, invalid requests; 100 frames %.3fs %.2ffps\n",w,h,seconds,100/seconds);
        return 0;
    } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}

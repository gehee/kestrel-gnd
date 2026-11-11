#pragma once
#include "hal/artosyn/ge2d_compositor.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <drm.h>
#include <drm_mode.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <utility>

inline void check(bool ok, const char *what) {
    if (!ok)
        throw std::system_error(errno, std::generic_category(), what);
}
namespace hal {
class DmaSurface {
  public:
    int drm = -1, fd = -1;
    uint32_t handle = 0, stride = 0;
    size_t bytes = 0;
    uint8_t *data = nullptr;
    DmaSurface(unsigned width, unsigned height, unsigned bpp) {
        try {
            drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
            check(drm >= 0, "open DRM");
            drm_mode_create_dumb create{};
            create.width = width;
            create.height = height;
            create.bpp = bpp;
            check(ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0, "allocate DMA surface");
            handle = create.handle;
            stride = create.pitch;
            bytes = create.size;
            drm_prime_handle prime{};
            prime.handle = handle;
            prime.flags = DRM_CLOEXEC | DRM_RDWR;
            check(ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0, "export DMA surface");
            fd = prime.fd;
            drm_mode_map_dumb map{};
            map.handle = handle;
            check(ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "map DMA surface");
            void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, drm, map.offset);
            check(p != MAP_FAILED, "mmap DMA surface");
            data = static_cast<uint8_t *>(p);
        } catch (...) {
            release();
            throw;
        }
    }
    DmaSurface(const DmaSurface &) = delete;
    DmaSurface &operator=(const DmaSurface &) = delete;
    ~DmaSurface() { release(); }
    void release() {
        if (data)
            munmap(data, bytes);
        if (fd >= 0)
            close(fd);
        if (handle) {
            drm_mode_destroy_dumb d{};
            d.handle = handle;
            ioctl(drm, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        if (drm >= 0)
            close(drm);
    }
    void sync(uint64_t flags) {
        dma_buf_sync s{flags};
        check(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0, "DMA CPU ownership");
    }
    ar_ge2d_image yuv(unsigned width, unsigned height, unsigned coded_height) const {
        ar_ge2d_image v{};
        v.width = width;
        v.height = height;
        v.format = AR_GE2D_YUV420;
        for (int i = 0; i < 3; ++i) {
            v.fd[i] = fd;
            v.stride[i] = stride / (i ? 2 : 1);
        }
        v.offset[1] = stride * coded_height;
        v.offset[2] = stride * coded_height * 5 / 4;
        return v;
    }
};
} // namespace hal

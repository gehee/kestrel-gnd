#include "hal/artosyn/ge2d_compositor.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hal {
static_assert(sizeof(ar_ge2d_image) == 48, "GE2D image ABI");
static_assert(sizeof(ar_ge2d_overlay) == 72, "GE2D overlay ABI");
static_assert(sizeof(ar_ge2d_compose) == 688, "GE2D compose ABI");

ar_ge2d_compose ge2d_request(const ar_ge2d_image &video, const ar_ge2d_image &output,
                             const std::vector<Ge2dOverlay> &overlays) {
    if (!video.width || !video.height || video.width > 1920 || video.height > 1080 ||
        ((video.width | video.height) & 1) || video.format != AR_GE2D_YUV420 ||
        output.format != AR_GE2D_YUV420 || video.width != output.width || video.height != output.height)
        throw std::invalid_argument("GE2D requires matching, even-sized YUV420 pictures up to 1920x1080");
    ar_ge2d_compose request{};
    request.version = AR_GE2D_ABI_VERSION;
    request.video = video;
    request.output = output;
    for (const auto &panel : overlays) {
        if (panel.image.format != AR_GE2D_ARGB8888 || !panel.image.width || !panel.image.height ||
            panel.image.width > 1920 || panel.image.height > 1080)
            throw std::invalid_argument("Invalid GE2D HUD panel");
        const int64_t x = panel.x, y = panel.y;
        const int64_t left = std::max<int64_t>(0, x), top = std::max<int64_t>(0, y);
        const int64_t right = std::min<int64_t>(output.width, x + panel.image.width);
        const int64_t bottom = std::min<int64_t>(output.height, y + panel.image.height);
        if (left >= right || top >= bottom)
            continue;
        if (request.count == AR_GE2D_MAX_OVERLAYS)
            throw std::invalid_argument("Too many visible GE2D HUD panels");
        auto &o = request.overlays[request.count++];
        o.image = panel.image;
        o.src_x = uint32_t(left - x);
        o.src_y = uint32_t(top - y);
        o.dst_x = uint32_t(left);
        o.dst_y = uint32_t(top);
        o.width = uint32_t(right - left);
        o.height = uint32_t(bottom - top);
    }
    return request;
}

Ge2dCompositor::Ge2dCompositor(const char *device) {
    fd_ = open(device, O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
        throw std::system_error(errno, std::generic_category(), "Open HUD compositor");
    ar_ge2d_caps caps{};
    if (ioctl(fd_, AR_GE2D_GET_CAPS, &caps) < 0) {
        const int error = errno;
        close(fd_);
        fd_ = -1;
        throw std::system_error(error, std::generic_category(), "Query HUD compositor");
    }
    if (caps.version != AR_GE2D_ABI_VERSION || caps.max_width < 1920 ||
        caps.max_height < 1080 || caps.max_overlays < AR_GE2D_MAX_OVERLAYS) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("Unsupported HUD compositor ABI");
    }
}
Ge2dCompositor::~Ge2dCompositor() {
    if (fd_ >= 0)
        close(fd_);
}
void Ge2dCompositor::compose(const ar_ge2d_image &video, const ar_ge2d_image &output,
                             const std::vector<Ge2dOverlay> &overlays) {
    const auto request = ge2d_request(video, output, overlays);
    // Do not retry interrupted submissions: a future ABI may complete a job
    // before reporting EINTR. The caller can cancel/export a fresh frame.
    if (ioctl(fd_, AR_GE2D_COMPOSE, &request) < 0)
        throw std::system_error(errno, std::generic_category(), "Compose HUD export frame");
}
} // namespace hal

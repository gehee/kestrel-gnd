#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
namespace hal {
// The node of a memory-to-memory encoder that writes HEVC: found by what it
// does, not by its number (the decoder, a USB camera or an analog capture
// driver can take any /dev/videoN first). Empty when there is none.
inline std::string find_hevc_encoder() {
    for (int n = 0; n < 64; ++n) {
        const auto node = "/dev/video" + std::to_string(n);
        const int fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        v4l2_capability cap{};
        bool found = false;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            const uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
            if (caps & V4L2_CAP_VIDEO_M2M_MPLANE) {
                v4l2_fmtdesc d{};
                d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                for (d.index = 0; !found && ioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; ++d.index)
                    found = d.pixelformat == V4L2_PIX_FMT_HEVC;
            }
        }
        close(fd);
        if (found)
            return node;
    }
    return {};
}
} // namespace hal

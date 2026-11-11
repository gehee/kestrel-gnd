#include "hal/artosyn/ge2d_compositor.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <climits>
#include <stdexcept>

int main() {
    ar_ge2d_image video{};
    video.width = 1920; video.height = 1080; video.format = AR_GE2D_YUV420;
    video.fd[0] = video.fd[1] = video.fd[2] = 4;
    video.stride[0] = 1920; video.stride[1] = video.stride[2] = 960;
    video.offset[1] = 1920 * 1088; video.offset[2] = 1920 * 1088 * 5 / 4;
    auto output = video;
    output.fd[0] = output.fd[1] = output.fd[2] = 5;
    hal::Ge2dOverlay panel;
    panel.image.width = 320; panel.image.height = 160; panel.image.format = AR_GE2D_ARGB8888;
    panel.image.fd[0] = 6; panel.image.stride[0] = 320 * 4;
    panel.x = -13; panel.y = -7;
    auto r = hal::ge2d_request(video, output, {panel});
    assert(r.version == 1 && r.count == 1 && !r.flags && !r.reserved);
    assert(r.video.offset[1] == 1920 * 1088); // preserve decoder padding
    assert(r.overlays[0].src_x == 13 && r.overlays[0].src_y == 7);
    assert(r.overlays[0].dst_x == 0 && r.overlays[0].dst_y == 0);
    assert(r.overlays[0].width == 307 && r.overlays[0].height == 153);
    panel.x = 1901; panel.y = 1075;
    r = hal::ge2d_request(video, output, {panel});
    assert(r.overlays[0].width == 19 && r.overlays[0].height == 5);
    assert(!r.overlays[0].src_x && !r.overlays[0].src_y);
    for (int x : {INT_MIN, INT_MAX, -320, 1920}) {
        panel.x = x;
        assert(hal::ge2d_request(video, output, {panel}).count == 0);
    }
    panel.x = 0; panel.y = 0;
    bool rejected = false;
    try { hal::ge2d_request(video, output, std::vector<hal::Ge2dOverlay>(9, panel)); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    output.width = 1280; rejected = false;
    try { hal::ge2d_request(video, output, {}); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    // Capability errors are surfaced and do not masquerade as a ready backend.
    rejected = false;
    try { hal::Ge2dCompositor unavailable("/dev/null"); }
    catch (const std::exception &) { rejected = true; }
    assert(rejected);
}

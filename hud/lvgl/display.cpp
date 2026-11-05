#include "display.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/fb.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace kestrel_ui {
Display::Display(int w, int h, const std::string& framebuffer, int draw_rows) : width_(w), height_(h) {
    if (draw_rows < 1 || draw_rows > 512) throw std::invalid_argument("Invalid partial draw-buffer height");
    if (!framebuffer.empty()) {
        fd_ = open(framebuffer.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) throw std::runtime_error("Cannot open framebuffer: " + std::string(std::strerror(errno)));
        try {
            if (flock(fd_, LOCK_EX | LOCK_NB)) throw std::runtime_error("Framebuffer already locked by another KestrelUI");
            fb_fix_screeninfo fix{};
            fb_var_screeninfo var{};
            if (ioctl(fd_, FBIOGET_FSCREENINFO, &fix) || ioctl(fd_, FBIOGET_VSCREENINFO, &var))
                throw std::runtime_error("Cannot read framebuffer geometry");
            if (var.bits_per_pixel != 32 || var.red.length != 8 || var.green.length != 8 ||
                var.blue.length != 8 || var.transp.length != 8 || var.grayscale ||
                fix.type != FB_TYPE_PACKED_PIXELS || fix.visual != FB_VISUAL_TRUECOLOR ||
                var.red.msb_right || var.green.msb_right || var.blue.msb_right || var.transp.msb_right)
                throw std::runtime_error("Framebuffer must expose packed 32-bit RGBA channels including alpha");
            const unsigned offsets[] = {var.red.offset, var.green.offset, var.blue.offset, var.transp.offset};
            unsigned mask = 0;
            for (unsigned offset : offsets) {
                if (offset > 24 || offset % 8 || (mask & (1u << (offset / 8))))
                    throw std::runtime_error("Unsupported framebuffer channel ordering");
                mask |= 1u << (offset / 8);
            }
            width_ = int(var.xres); height_ = int(var.yres);
            stride_ = fix.line_length; xoffset_ = var.xoffset; yoffset_ = var.yoffset;
            red_ = var.red.offset; green_ = var.green.offset; blue_ = var.blue.offset; alpha_ = var.transp.offset;
            mapping_size_ = fix.smem_len;
            const uint64_t end = uint64_t(yoffset_ + var.yres - 1) * stride_ + uint64_t(xoffset_ + var.xres) * 4;
            if (width_ < 1 || height_ < 1 || width_ > 4096 || height_ > 2160 ||
                uint64_t(xoffset_) + width_ > var.xres_virtual || uint64_t(yoffset_) + height_ > var.yres_virtual ||
                uint64_t(xoffset_ + var.xres) * 4 > stride_ || end > mapping_size_)
                throw std::runtime_error("Invalid framebuffer stride, offsets or memory bounds");
            void* map = mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (map == MAP_FAILED) throw std::runtime_error("Cannot mmap framebuffer");
            mapping_ = static_cast<uint8_t*>(map);
            backup_.resize(size_t(width_) * height_);
            for (int y = 0; y < height_; ++y)
                std::memcpy(backup_.data() + size_t(y) * width_, mapping_ + size_t(y + yoffset_) * stride_ + xoffset_ * 4, width_ * 4);
            std::printf("FB size=%dx%d stride=%u offset=%u,%u channels=%u,%u,%u,%u mapped=%zu\n",
                        width_, height_, stride_, xoffset_, yoffset_, red_, green_, blue_, alpha_, mapping_size_);
        } catch (...) {
            if (mapping_) munmap(mapping_, mapping_size_);
            close(fd_); fd_ = -1; mapping_ = nullptr;
            throw;
        }
    }
    // A larger partial buffer reduces repeated custom DRAW callbacks during
    // menu animation. 192 rows costs ~1.4 MiB at 1080p, still no full-frame
    // draw buffer or GPU context. The retained surface supports screenshots.
    pixels_.resize(size_t(width_) * height_);
    draw_buffer_.resize(size_t(width_) * draw_rows);
    display_ = lv_display_create(width_, height_);
    if (!display_) throw std::runtime_error("Cannot create LVGL display");
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(display_, draw_buffer_.data(), nullptr, draw_buffer_.size() * 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display_, this);
    lv_display_set_flush_cb(display_, flush_cb);
}
Display::~Display() {
    if (display_) lv_display_delete(display_);
    if (mapping_) {
        for (int y = 0; y < height_; ++y)
            std::memcpy(mapping_ + size_t(y + yoffset_) * stride_ + xoffset_ * 4, backup_.data() + size_t(y) * width_, width_ * 4);
        munmap(mapping_, mapping_size_);
    }
    if (fd_ >= 0) close(fd_);
}
void Display::flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    auto* self = static_cast<Display*>(lv_display_get_user_data(display));
    self->flush(*area, pixels);
    if (lv_display_flush_is_last(display)) ++self->presentations;
    lv_display_flush_ready(display);
}
void Display::flush(const lv_area_t& a, const uint8_t* source) {
    if (a.x1 < 0 || a.y1 < 0 || a.x2 >= width_ || a.y2 >= height_ || a.x2 < a.x1 || a.y2 < a.y1) return;
    const size_t row_bytes = size_t(a.x2 - a.x1 + 1) * 4;
    for (int y = a.y1; y <= a.y2; ++y) {
        auto* dest = pixels_.data() + size_t(y) * width_ + a.x1;
        std::memcpy(dest, source, row_bytes);
        if (mapping_) {
            auto* fb = mapping_ + size_t(y + yoffset_) * stride_ + size_t(a.x1 + xoffset_) * 4;
            if (red_ == 16 && green_ == 8 && blue_ == 0 && alpha_ == 24) std::memcpy(fb, source, row_bytes);
            else for (size_t x = 0; x < row_bytes / 4; ++x) {
                const uint32_t p = dest[x];
                const uint32_t converted = ((p >> 16) & 255) << red_ | ((p >> 8) & 255) << green_ |
                                           (p & 255) << blue_ | (p >> 24) << alpha_;
                std::memcpy(fb + x * 4, &converted, 4);
            }
        }
        source += row_bytes;
        bytes_written += row_bytes;
    }
    ++flushes;
}
void Display::dump(const std::string& path) const {
    // PAM keeps alpha; readable by Pillow/ImageMagick without a target PNG lib.
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open screenshot output");
    out << "P7\nWIDTH " << width_ << "\nHEIGHT " << height_ << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    for (uint32_t p : pixels_) {
        const char rgba[] = {char(p >> 16), char(p >> 8), char(p), char(p >> 24)};
        out.write(rgba, 4);
    }
    if (!out) throw std::runtime_error("Screenshot write failed");
}
void Display::verify_framebuffer() const {
    if (!mapping_) return;
    size_t mismatches = 0;
    for (int y = 0; y < height_; ++y) for (int x = 0; x < width_; ++x) {
        const uint32_t p = pixels_[size_t(y) * width_ + x];
        const uint32_t expected = ((p >> 16) & 255) << red_ | ((p >> 8) & 255) << green_ |
                                  (p & 255) << blue_ | (p >> 24) << alpha_;
        uint32_t actual;
        std::memcpy(&actual, mapping_ + size_t(y + yoffset_) * stride_ + size_t(x + xoffset_) * 4, 4);
        if (actual != expected) ++mismatches;
    }
    std::printf("FB_READBACK pixels=%zu mismatches=%zu\n", pixels_.size(), mismatches);
    if (mismatches) throw std::runtime_error("Framebuffer readback differs from rendered overlay");
}
} // namespace kestrel_ui

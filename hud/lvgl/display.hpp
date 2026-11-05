#pragma once
#include "lvgl.h"
#include <cstdint>
#include <string>
#include <vector>

namespace kestrel_ui {
// CPU overlay presentation only. Never changes modes, pans, unblanks, loads a
// module or initializes the media service. Copies damaged areas to the currently
// selected framebuffer page; its visible contents are restored on normal exit.
class Display {
public:
    Display(int width, int height, const std::string& framebuffer, int draw_rows = 192);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    int width() const { return width_; }
    int height() const { return height_; }
    lv_display_t* lv_display() const { return display_; }
    const std::vector<uint32_t>& pixels() const { return pixels_; }
    void dump(const std::string& path) const;
    void verify_framebuffer() const;
    uint64_t flushes = 0, bytes_written = 0, presentations = 0;
private:
    int width_, height_, fd_ = -1;
    uint32_t stride_ = 0, xoffset_ = 0, yoffset_ = 0;
    unsigned red_ = 16, green_ = 8, blue_ = 0, alpha_ = 24;
    uint8_t* mapping_ = nullptr;
    size_t mapping_size_ = 0;
    lv_display_t* display_ = nullptr;
    std::vector<uint32_t> pixels_, draw_buffer_, backup_;
    void flush(const lv_area_t& area, const uint8_t* pixels);
    static void flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);
};
} // namespace kestrel_ui

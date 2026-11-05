#pragma once
#include "hud_protocol.hpp"

namespace kestrel_ui {
class HudBuffer {
public:
    HudBuffer(const hud_ipc::Layout& layout, int supplied_fd = -1);
    ~HudBuffer();
    HudBuffer(const HudBuffer&) = delete;
    HudBuffer& operator=(const HudBuffer&) = delete;
    int fd() const { return fd_; }
    uint8_t* region(uint32_t slot, size_t index) const;
    const hud_ipc::Layout& layout() const { return layout_; }
private:
    hud_ipc::Layout layout_;
    int fd_ = -1;
    uint8_t* mapping_ = nullptr;
    size_t size_ = 0;
};
} // namespace kestrel_ui

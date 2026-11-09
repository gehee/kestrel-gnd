#pragma once
#include "hud_buffer.hpp"
#include <memory>

namespace kestrel_ui {
enum class HudStyle { Canopy, Vector };
class HudRenderer {
public:
    virtual ~HudRenderer() = default;
    virtual const hud_ipc::Layout& layout() const = 0;
    virtual void render(const HudData& data, HudBuffer& buffer, uint32_t slot) = 0;
    virtual size_t cache_bytes() const = 0;
    virtual uint64_t rasters() const = 0;
};
std::unique_ptr<HudRenderer> make_hud_renderer(HudStyle style, int width, int height, const std::string& assets);
} // namespace kestrel_ui

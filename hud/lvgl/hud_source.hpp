#pragma once
#include "hud_protocol.hpp"
#include "hud_renderer.hpp"
#include "lvgl.h"
#include <memory>
#include <string>

namespace kestrel_ui {
// UI renderer adapter. Wire/domain types remain graphics-independent; LVGL
// image descriptors are confined to this software compositor implementation.
class HudSource {
public:
    virtual ~HudSource() = default;
    virtual void update(const HudData& data, uint64_t now) = 0;
    virtual const hud_ipc::Layout& layout() const = 0;
    virtual const lv_image_dsc_t* image(size_t region) const = 0;
    virtual uint64_t revision() const = 0;
    virtual uint32_t changed_regions() const = 0;
    virtual size_t cache_bytes() const = 0;
    virtual uint64_t rasters() const = 0;
};
// Synchronous adapter for pixel/regression tests and offscreen benchmarks.
// It uses the exact same passive View as the separate executable.
class InlineHud final : public HudSource {
public:
    InlineHud(int width, int height, const std::string& assets, HudStyle style = HudStyle::Canopy);
    ~InlineHud() override;
    void update(const HudData& data, uint64_t now) override;
    const hud_ipc::Layout& layout() const override;
    const lv_image_dsc_t* image(size_t region) const override;
    uint64_t revision() const override;
    uint32_t changed_regions() const override;
    size_t cache_bytes() const override;
    uint64_t rasters() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
class HudClient final : public HudSource {
public:
    HudClient(int width, int height, const std::string& assets, const std::string& executable);
    ~HudClient() override;
    void update(const HudData& data, uint64_t now) override;
    const hud_ipc::Layout& layout() const override;
    const lv_image_dsc_t* image(size_t region) const override;
    uint64_t revision() const override;
    uint32_t changed_regions() const override;
    size_t cache_bytes() const override;
    uint64_t rasters() const override;
    int pid() const;
    uint64_t restarts() const;
    uint64_t frames() const;
    uint64_t render_us() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace kestrel_ui

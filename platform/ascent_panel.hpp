#pragma once
#include "platform/native_panel.hpp"
#include <memory>
#include <string>
#include <vector>

namespace platform {
// Owns Linux panel resources. No UI or radio objects cross the worker boundary.
class AscentPanel : public NativePanel {
  public:
    AscentPanel();
    ~AscentPanel() override;
    AscentPanel(const AscentPanel &) = delete;
    AscentPanel &operator=(const AscentPanel &) = delete;
    std::vector<PanelEvent> take() override;
    void beep(PanelTone tone = PanelTone::Click) override;
    std::string sources() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace platform

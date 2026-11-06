#pragma once
#include "platform/panel_logic.hpp"
#include <memory>
#include <string>
#include <vector>

namespace platform {
enum class PanelTone { Click, Notice, Success, Failure };
// Owns Linux panel resources. No UI or radio objects cross the worker boundary.
class AscentPanel {
  public:
    AscentPanel();
    ~AscentPanel();
    AscentPanel(const AscentPanel &) = delete;
    AscentPanel &operator=(const AscentPanel &) = delete;
    std::vector<PanelEvent> take();
    void beep(PanelTone tone = PanelTone::Click);
    std::string sources() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace platform

#include "hud_renderer.hpp"
#include "hud_view.hpp"
#include "vector_hud.hpp"
namespace kestrel_ui {
std::unique_ptr<HudRenderer> make_hud_renderer(HudStyle style, int w, int h, const std::string& assets) {
    if (style == HudStyle::Vector) return std::make_unique<VectorHud>(w, h, assets);
    return std::make_unique<HudView>(w, h, assets);
}
}

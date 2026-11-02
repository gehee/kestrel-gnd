// A box whose screen is KMS's and whose HUD is drawn with GLES - the VRX Pro
// and a PC: the display (display/drm), its presenter (display/renderer), the
// GL context and the HUD's layer (platform/gles_hud_layer).
#include "display/drm.hpp"
#include "display/renderer.hpp"
#include "platform/gles_hud_layer.hpp"
#include "platform/platform.hpp"
#include "render/canvas_gles.hpp"
#include "render/gl_context.hpp"

namespace platform {

namespace {
// The display these boxes make is KMS's (make_display below).
DrmDevice& kms(display::Display& d) { return static_cast<DrmDevice&>(d); }
}  // namespace

std::shared_ptr<display::Display> make_display(Settings& settings, const DisplaySetup& s) {
    auto dev = std::make_shared<DrmDevice>(settings);
    dev->display_id = s.screen_id;   // a different screen plugged in restarts us
    dev->follow_camera = s.follow_camera;
    dev->follow_fps = s.follow_fps;
    dev->follow_lines = s.follow_lines;
    dev->set_sync_mode(s.sync_mode);   // before init: Auto's first mode depends on it
    dev->set_sync_slice(s.sync_slice);
    dev->set_sync_margin_us(s.sync_margin_us);
    dev->init(s.width, s.height, s.refresh, s.video_zpos, s.osd_zpos, s.vrr);
    return dev;
}

bool init_graphics(display::Display& d, bool msaa) {
    return GlContext::instance().init(kms(d).drm_fd, msaa);
}

std::unique_ptr<render::Canvas> make_canvas() {
    return std::unique_ptr<render::Canvas>(new render::CanvasGles());
}

std::unique_ptr<display::HudLayer> make_hud_layer(display::Display& d) {
    return std::unique_ptr<display::HudLayer>(new GlesHudLayer(kms(d)));
}

std::shared_ptr<display::Presenter> make_presenter(std::shared_ptr<display::Display> d, RenderMode mode,
                                                   Settings& settings, std::function<void(int, int)> on_frame_size,
                                                   std::shared_ptr<display::VideoListener> listener,
                                                   std::atomic<bool>* stop, bool console_stats) {
    auto dev = std::static_pointer_cast<DrmDevice>(d);
    auto r = std::make_shared<Renderer>(mode, dev, settings, std::move(on_frame_size), std::move(listener), stop,
                                        console_stats);
    dev->flips_are_frames_ = (mode == Atomic);   // the stats screen reads flip timing as smoothness
    return r;
}

}  // namespace platform

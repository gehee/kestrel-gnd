#include "model.hpp"

#include "hud/hud_theme.hpp"
#include "font_atlas.hpp"
#include "input.hpp"
#include "preferences.hpp"
#include "render/software/frame_cadence.hpp"
#include "utils/math_utils.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <unistd.h>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void le32(std::vector<uint8_t>& out, size_t offset, int32_t value) {
    for (size_t i = 0; i < 4; ++i) out[offset + i] = uint8_t(uint32_t(value) >> (i * 8));
}
canopy::Point gles_projection(int width, int height, bool left, float x, float y) {
    // Independently exercise the legacy OSD's full matrix chain. This catches
    // incorrect software yaw/slant/pivot or screen-y conventions without GL.
    constexpr float depth = 3.2f;
    const float fh = std::tan(0.785f / 2) * depth, fw = fh * width / height;
    const auto f = canopy::canopy_frame(fw, fh, 0.02f, 1, depth);
    math::Mat4 projection, view, vp, model, neg{}, pos{}, rotation, a, b, m, mvp;
    math::perspective(projection, 0.785f, float(width) / height, 0.1f, 100);
    math::lookAt(view, {0, 0, depth}, {0, 0, 0}, {0, 1, 0});
    math::multiply(vp, view, projection);
    math::rotateY(model, left ? 0.26f : -0.26f);
    neg[0] = neg[5] = neg[10] = neg[15] = 1;
    pos[0] = pos[5] = pos[10] = pos[15] = 1;
    pos[12] = left ? f.LXo(56) : f.RXo(56); pos[13] = f.MY(502);
    neg[12] = -pos[12]; neg[13] = -pos[13];
    math::rotateZ(rotation, left ? canopy::kSlantRad : -canopy::kSlantRad);
    math::multiply(a, neg, rotation); math::multiply(b, a, pos);
    math::multiply(m, b, model); math::multiply(mvp, m, vp);
    const float px = left ? f.LXo(x) : f.RXo(x), py = f.MY(y);
    const float cx = mvp[0] * px + mvp[4] * py + mvp[12];
    const float cy = mvp[1] * px + mvp[5] * py + mvp[13];
    const float cw = mvp[3] * px + mvp[7] * py + mvp[15];
    return {width * (0.5f + 0.5f * cx / cw), height * (0.5f - 0.5f * cy / cw)};
}
}
int main(int argc, char** argv) {
    using namespace kestrel_ui;
    try {
        check(next_frame_deadline(1000, 1090, 100) == 1100, "Small render/poll jitter does not move cadence phase");
        check(next_frame_deadline(1000, 1100, 100) == 1200, "A missed exact deadline is skipped, not retried");
        check(next_frame_deadline(1000, 1435, 100) == 1500, "Long overruns skip stale animation poses");
        Telemetry t;
        t.stamp_us = 1000000; t.connected = true; t.mcs = -1;
        t.voltage_mv = 11512; t.cpu_temperature_x100 = 4889;
        check(!t.fresh(3000000) && !t.fresh(999999), "Telemetry freshness bounds");

        Model model;
        Controller controller(model);
        model.backend.vrx = t;
        controller.tick(1000100);
        check(model.status(1000100) == "LINK UP / NO VALID MCS", "No fabricated streaming state");
        model.backend.vrx.mcs = 4;
        controller.tick(1000200);
        check(model.visual.visible && model.visual.animating && model.visual.rule == 0, "Acquisition start");
        controller.tick(1420200);
        check(!model.visual.animating && model.visual.block == 1, "Acquisition settles");
        model.backend.vrx.connected = false;
        controller.tick(1500000);
        controller.tick(1740000);
        check(std::abs(model.visual.idle_dim - 0.5f) < 0.001, "Original idle dim timing");
        controller.tick(2300000);
        check(model.visual.mark == 1 && !model.visual.animating, "Original idle transition settles");
        controller.key(Key::Enter);
        check(model.menu_open && model.stage == MenuStage::Pick, "OK wakes the canopy picker");
        controller.key(Key::Right); check(model.side == 1, "Pick air unit");
        controller.key(Key::Left); controller.key(Key::Enter);
        check(model.stage == MenuStage::Browse && model.tab == 2, "Open the ground HUD blade");
        controller.key(Key::Right); // DISPLAY > Theme
        controller.key(Key::Enter); controller.key(Key::Down);
        check(model.stage == MenuStage::Edit && model.theme == 1, "Preview local theme in an edit transaction");
        controller.key(Key::Back);
        check(model.stage == MenuStage::Browse && model.theme == 1 && model.pending("theme"), "Back folds an edit and keeps the pending preview like VRX Pro");
        controller.key(Key::Enter); controller.key(Key::Enter);
        check(model.stage == MenuStage::Browse && model.theme == 1 && !model.pending("theme"), "OK commits the preview");
        for (int i = 0; i < 40; ++i) controller.key(Key::Down);
        check(model.selected < int(model.rows(1000000).size()), "Dynamic row bounds");
        controller.key(Key::Back); check(model.stage == MenuStage::Pick && model.menu_open, "Back folds the blade first");
        controller.key(Key::Back); check(!model.menu_open, "Back then closes the canopy");
        controller.key(Key::Enter); controller.key(Key::Enter);
        check(model.tab == 3, "Reopening remembers DISPLAY on the selected side");
        controller.key(Key::Enter); controller.key(Key::Down); check(model.theme == 2, "Theme preview");
        model.tab = 2; controller.key(Key::Enter);
        check(model.theme == 1 && model.stage == MenuStage::Browse, "Changing rows during editing cancels; it cannot activate another action");
        model.tab = 3; model.selected = 4; controller.key(Key::Down);
        check(model.selected == 6, "Navigation skips the ADVANCED group heading");
        model.selected = 1; controller.key(Key::Enter);
        check(model.stage == MenuStage::Browse, "Unavailable controls cannot start editing");
        model.tab = 2; model.selected = 0; controller.key(Key::Enter); controller.key(Key::Up); controller.key(Key::Enter);
        check(!model.show_hud && controller.hud_data(2300000).panels[0].visible, "HUD visibility setting applies");
        controller.key(Key::Menu);
        check(!controller.hud_data(2300000).panels[0].visible, "HUD OFF hides the canopy outside the menu");
        model.show_hud = true;
        // Applying one setting must not accidentally save a different row's
        // unfinished preview. Both may be visible while browsing the menu.
        Model pending_model; Controller pending_controller(pending_model);
        UiPreferences saved;
        pending_controller.save_preferences = [&](const UiPreferences& p) { saved = p; };
        pending_controller.key(Key::Enter); pending_controller.key(Key::Enter);
        pending_controller.key(Key::Enter); pending_controller.key(Key::Up); pending_controller.key(Key::Back);
        pending_controller.key(Key::Right); pending_controller.key(Key::Enter);
        pending_controller.key(Key::Down); pending_controller.key(Key::Enter);
        check(saved.hud && saved.theme == 1 && pending_model.pending("hud"), "Applying Theme does not save pending HUD OFF");
        pending_controller.key(Key::Left); pending_controller.key(Key::Enter); pending_controller.key(Key::Enter);
        check(!saved.hud && saved.theme == 1 && !pending_model.pending("hud"), "A pending row is applied independently");
        Model motion_model; Controller motion_controller(motion_model);
        motion_model.dynamic=2;
        UiPreferences motion_saved;int motion_saves=0;
        motion_controller.save_preferences=[&](const UiPreferences& p){motion_saved=p;++motion_saves;};
        motion_controller.key(Key::Enter);motion_controller.key(Key::Enter);
        const auto motion_rows=motion_model.rows(2300000);
        const auto dynamic_row=std::find_if(motion_rows.begin(),motion_rows.end(),[](const auto& row){return row.id=="dynamic";});
        check(dynamic_row!=motion_rows.end(),"Dynamic HUD setting is present");
        motion_model.selected=int(dynamic_row-motion_rows.begin());
        motion_controller.key(Key::Enter);motion_controller.key(Key::Up);motion_controller.key(Key::Up);
        check(motion_model.dynamic==0,"Dynamic HUD OFF previews through menu input");
        motion_controller.key(Key::Enter);motion_controller.key(Key::Back);motion_controller.key(Key::Back);
        check(motion_model.dynamic==0 && motion_saved.dynamic==0 && motion_saves==1,"Dynamic HUD OFF commits and stays disabled after closing menu");
        Model reloaded_motion;reloaded_motion.set_preferences(motion_saved);
        check(reloaded_motion.dynamic==0,"Reloading preferences preserves Dynamic HUD OFF");
        Model failure;
        Controller failure_controller(failure);
        failure_controller.save_preferences = [](const UiPreferences&) { throw std::runtime_error("read-only config"); };
        failure_controller.key(Key::Enter); failure_controller.key(Key::Enter); failure_controller.key(Key::Right); failure_controller.key(Key::Enter); failure_controller.key(Key::Down); failure_controller.key(Key::Enter);
        check(failure.theme == 0 && failure.feedback.find("Save failed") != std::string::npos, "Failed persistence rolls back the preview");
        Key parsed;
        check(parse_key(" OK\n", parsed) && parsed == Key::Enter && !parse_key("bind", parsed), "Named controls are restricted to UI keys");
        check(parse_key("rec",parsed) && parsed==Key::Record,"REC maps to the gallery record button");
        {
            Model gallery;Controller gc(gallery);gc.set_scene(SceneMode::GalleryBrowse);
            gallery.backend.recordings={{"one.mp4",1},{"two.mp4",2}};
            std::string action;
            gc.apply_receiver=[&](const std::string& id,int){action=id;return std::string("Action requested");};
            gc.key(Key::Record);gc.key(Key::Record,true);
            check(gallery.gallery_action_name=="one.mp4" && gallery.gallery_delete_name.empty(),"REC opens actions, not deletion; held REC is ignored");
            check(gallery.gallery_action==RecordingAction::Export,"Actions default to export");
            gc.key(Key::Enter);
            check(action.empty() && gallery.feedback=="HUD export unavailable" && !gallery.gallery_action_name.empty(),"Unimplemented export is visible but cannot dispatch");
            gc.key(Key::Down);gc.key(Key::Down,true);
            check(gallery.gallery_selected==0 && gallery.gallery_action==RecordingAction::Delete,"Actions arrows do not move gallery selection");
            gc.key(Key::Enter);
            check(gallery.gallery_delete_name=="one.mp4" && !gallery.gallery_delete_chosen,"Delete action opens confirmation on Cancel");
            gc.key(Key::Enter);
            check(action.empty() && gallery.gallery_delete_name.empty() && !gallery.gallery_action_name.empty(),"Cancel returns to recording actions");
            gc.key(Key::Enter);gc.key(Key::Right);gc.key(Key::Back);
            check(action.empty() && gallery.gallery_delete_name.empty() && gallery.scene==SceneMode::GalleryBrowse,"Back cancels confirmation without leaving actions");
            gc.key(Key::Back);check(gallery.gallery_action_name.empty(),"Back closes actions");
            gc.key(Key::Record);gc.key(Key::Right);gc.key(Key::Enter);gc.key(Key::Right);gc.key(Key::Left);gc.key(Key::Enter);
            check(action.empty(),"LEFT selects Cancel before confirming deletion");
            gc.key(Key::Back);gc.key(Key::Record);
            std::reverse(gallery.backend.recordings.begin(),gallery.backend.recordings.end());
            gc.key(Key::Down);gc.key(Key::Enter);gc.key(Key::Right);gc.key(Key::Enter);
            check(action=="delete:one.mp4" && gallery.gallery_delete_name.empty() && gallery.gallery_action_name.empty(),"Deletion remains pinned across catalog refresh and closes dialogs");
            gallery.backend.controls["recording_export"].known=true;
            gallery.backend.controls["recording_export"].enabled=true;
            gc.key(Key::Record);gc.key(Key::Down);gc.key(Key::Up);gc.key(Key::Enter);
            check(action=="export:two.mp4" && gallery.gallery_action_name.empty(),"Ready export dispatches selected filename");
            gc.apply_receiver=[](const std::string&,int)->std::string{throw std::runtime_error("Storage busy");};
            gc.key(Key::Record);gc.key(Key::Enter);
            check(gallery.feedback=="Storage busy" && !gallery.gallery_action_name.empty(),"Export errors keep actions available");
            gc.key(Key::Right);gc.key(Key::Enter);gc.key(Key::Right);gc.key(Key::Enter);
            check(gallery.feedback=="Storage busy" && gallery.gallery_action_name.empty(),"Deletion errors return to gallery");
            gc.key(Key::Record);gc.key(Key::Record);
            check(gallery.gallery_action_name.empty(),"REC toggles actions closed");
            gallery.backend.recordings.clear();gc.key(Key::Record);
            check(gallery.gallery_action_name.empty(),"Empty gallery cannot open actions");
            gallery.backend.recordings={{"one.mp4",1}};gc.key(Key::Record);
            gc.set_scene(SceneMode::GalleryPlayback);gc.key(Key::Record);
            check(gallery.gallery_action_name.empty() && gallery.gallery_delete_name.empty(),"Scene changes dismiss actions; REC does not delete during playback");
        }
        model.key_repeat = false; const auto keys_before = model.key_count;
        controller.key(Key::Up, true); check(model.key_count == keys_before, "Disabled repeats do not move the menu");
        model.key_repeat = true; controller.key(Key::Menu, true); check(model.key_count == keys_before, "Menu keys never auto-repeat");
        controller.set_scene(SceneMode::GalleryBrowse);
        check(!model.hud_visible() && !controller.hud_data(2300000).enabled, "Gallery browsing hides the passive HUD");
        controller.set_scene(SceneMode::GalleryPlayback);
        check(!model.hud_visible(), "Legacy playback hides live HUD readings");
        auto recorded=std::make_shared<HudData>();recorded->panels[0].primary="16.40";
        model.backend.playback_hud=recorded;model.playback_hud_enabled=true;
        check(model.hud_visible() && controller.hud_data(2300000).panels[0].primary=="16.40", "Playback uses recorded HUD values");
        controller.key(Key::Up);check(!model.hud_visible() && !controller.hud_data(2300000).enabled,"Playback HUD can be hidden");
        controller.key(Key::Up);check(model.hud_visible(),"Playback HUD can be restored");
        controller.key(Key::Down);
        check(!model.playback_controls_visible && model.hud_visible(),"Down hides playback controls independently of HUD");
        controller.key(Key::Down,true);
        check(!model.playback_controls_visible,"Holding Down does not repeatedly toggle playback controls");
        controller.key(Key::Down);check(model.playback_controls_visible,"Down restores playback controls");
        controller.key(Key::Down);controller.set_scene(SceneMode::GalleryPlayback);
        check(model.playback_controls_visible,"New playback restores visible controls");
        model.backend.playback_hud.reset();
        std::string playback_action;int playback_value=0;
        controller.apply_receiver=[&](const std::string& id,int value){playback_action=id;playback_value=value;return std::string();};
        controller.key(Key::Enter);
        check(playback_action=="playback_pause", "OK toggles playback pause");
        controller.key(Key::Left);check(playback_action=="playback_seek" && playback_value==-5,"Left seeks five seconds backward");
        controller.key(Key::Right);check(playback_action=="playback_seek" && playback_value==5,"Right seeks five seconds forward");
        controller.key(Key::Right,true);check(playback_action=="playback_seek" && playback_value==5,"Held arrows repeat seeking");
        controller.key(Key::Back);
        check(model.scene_mode()==SceneMode::GalleryBrowse && playback_action=="playback_stop", "Back stops playback and restores the catalog");
        controller.key(Key::Back);
        check(model.scene_mode() == SceneMode::Live && model.hud_visible(), "Back from gallery restores live HUD visibility");
        model.backend.controls["gallery"].enabled=true;
        controller.key(Key::Back);
        check(model.scene_mode()==SceneMode::GalleryBrowse,"Back on live picture opens gallery like VRX Pro");
        controller.key(Key::Back);
        model.backend.flight.stamp_us = 3000000;
        model.backend.flight.have_rssi = true; model.backend.flight.rssi_pct = 45;
        controller.tick(3000000);
        check(controller.hud_data(3000000).panels[1].upper_label == "RC RSSI" &&
              controller.hud_data(3000000).panels[1].upper_level == 0.45f, "Controller supplies interpreted RC data to the passive HUD");
        const auto instruments = controller.hud_data(3000000);
        check(instruments.metric(HudMetricId::Rc)->valid && instruments.metric(HudMetricId::Rc)->value == "45" &&
              !instruments.metric(HudMetricId::Altitude)->valid && instruments.metric(HudMetricId::Altitude)->value == "--",
              "Reference-inspired instruments use available readings and explicit missing values");
        check(!controller.hud_data(5000000).metric(HudMetricId::Rc)->valid, "Instrument validity expires with source telemetry");

        TerminalDecoder terminal;
        std::vector<Key> keys;
        const KeySink receive_key = [&](Key key, bool) { keys.push_back(key); };
        terminal.feed("\x1b", 1, 1000, receive_key);
        terminal.feed("[", 1, 2000, receive_key);
        terminal.feed("1;2A", 4, 3000, receive_key);
        check(keys == std::vector<Key>{Key::Up}, "Fragmented/modifier arrow escape sequence");
        terminal.feed("\r", 1, 4000, receive_key); terminal.feed("\n", 1, 5000, receive_key);
        check(keys.size() == 2 && keys.back() == Key::Enter, "CRLF is one OK, not edit plus apply");
        terminal.feed("\x1b", 1, 6000, receive_key); terminal.expire(160001, receive_key);
        check(keys.back() == Key::Back, "Standalone escape backs out after a bounded wait");
        bool repeated;
        const uint8_t stock_up[] = {0x40, 0, 0}, debug[] = {0x40, 9, 0}, long_ok[] = {0x40, 4, 1};
        check(stock_key(stock_up, 3, parsed, repeated) && parsed == Key::Up && !repeated, "Validated stock menu-key mapping");
        check(!stock_key(debug, 3, parsed, repeated) && !stock_key(long_ok, 3, parsed, repeated), "Unmapped/debug/long stock keys are not actions");

        const char* temporary_root = std::getenv("TMPDIR");
        const std::string settings_pattern = std::string(temporary_root ? temporary_root : "/tmp") + "/kestrel-ui-settings-test-XXXXXX";
        std::vector<char> settings_directory(settings_pattern.begin(), settings_pattern.end()); settings_directory.push_back('\0');
        if (!mkdtemp(settings_directory.data())) throw std::runtime_error("Cannot create preferences test directory");
        const std::string settings_path = std::string(settings_directory.data()) + "/ui.conf";
        UiPreferences persisted{2, 30, false, false, false, false};persisted.dynamic=0;
        store_preferences(settings_path, persisted);
        const auto loaded = load_preferences(settings_path);
        check(loaded.theme == 2 && loaded.fps == 30 && !loaded.details && !loaded.repeat && !loaded.hud && !loaded.per_cell, "Local preferences round trip");
        check(loaded.dynamic==0,"Dynamic HUD OFF survives storage round trip");
        { std::ofstream invalid(settings_path); invalid << "UNRELATED_USER_FILE=1\n"; }
        bool invalid_settings = false;
        try { load_preferences(settings_path); } catch (const std::runtime_error&) { invalid_settings = true; }
        check(invalid_settings, "Unrelated configuration is not adopted/overwritten");
        unlink(settings_path.c_str()); rmdir(settings_directory.data());
        for (float x : {0.0f, 156.0f, 300.0f}) {
            auto l = canopy::project(1920, 1080, true, x, 526);
            auto r = canopy::project(1920, 1080, false, x, 526);
            check(std::abs(l.x + r.x - 1920) < 0.01 && std::abs(l.y - r.y) < 0.01, "Canopy projection mirror symmetry");
        }
        for (auto size : {canopy::Point{1920, 1080}, canopy::Point{1280, 720}})
            for (bool left : {true, false}) for (float x : {-60.0f, 14.0f, 156.0f, 252.0f, 300.0f})
                for (float y : {456.0f, 512.0f, 526.0f, 596.0f}) {
                    const auto p = canopy::project(int(size.x), int(size.y), left, x, y);
                    const auto ref = gles_projection(int(size.x), int(size.y), left, x, y);
                    check(std::abs(p.x - ref.x) < 0.01f && std::abs(p.y - ref.y) < 0.01f, "Software asset geometry matches GLES matrix chain");
                }
        canopy::AlphaSurface pattern{0, 0, 3, 2, {10, 20, 30, 40, 50, 60}};
        auto identity = canopy::warp_alpha(pattern, {{{2, 3}, {5, 3}, {5, 5}, {2, 5}}}, 10, 10);
        check(identity.x == 2 && identity.y == 3 && identity.width == 3 && identity.height == 2 && identity.pixels == pattern.pixels,
              "Identity warp preserves asymmetric texels without a triangle seam");
        auto clipped = canopy::warp_alpha(pattern, {{{-1, 0}, {2, 0}, {2, 2}, {-1, 2}}}, 10, 10);
        check(clipped.width == 2 && clipped.pixels == std::vector<uint8_t>({20, 30, 50, 60}), "Warp clipping preserves source offsets");
        for (bool left : {true, false}) {
            const auto quad = canopy::text_quad(1920, 1080, left, 14, 512, canopy::kTextBig, 96, 80, !left);
            check(quad[1].x > quad[0].x && quad[2].x > quad[3].x, "Right panel text is never mirrored");
            canopy::Homography map(quad);
            for (auto uv : {canopy::Point{0.2f, 0.3f}, canopy::Point{0.5f, 0.5f}, canopy::Point{0.8f, 0.7f}}) {
                const auto p = map.forward(uv.x, uv.y), back = map.inverse(p.x, p.y);
                check(std::abs(back.x - uv.x) < 0.0001 && std::abs(back.y - uv.y) < 0.0001, "Projective mapping round trip");
                const auto f = canopy::screen_frame(1920, 1080);
                float wx = left ? f.LXo(14) : f.RXo(14) - 80 * canopy::kTextBig / 64;
                const auto ref = canopy::project_world(1920, 1080, left, wx + uv.x * 96 * canopy::kTextBig / 64,
                                                      f.MY(512) + (1 - uv.y) * canopy::kTextBig);
                check(std::abs(ref.x - p.x) < 0.01 && std::abs(ref.y - p.y) < 0.01, "Glyph interior follows true 3D perspective, not affine interpolation");
            }
        }
        bool rejected = false;
        try { canopy::Homography invalid({{{0, 0}, {1, 1}, {0, 1}, {1, 0}}}); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Reject folded quads");

        std::string atlas;
        auto put = [&](uint32_t v) { for (int i = 0; i < 4; ++i) atlas += char(v >> (8 * i)); };
        for (uint32_t v : {0x4649554bu, 1u, 40u, 64u, 48u, 2u}) put(v);
        for (uint32_t c : {63u, 65u}) {
            put(c); put(8); put(256);
            std::string pixels(8 * 64, '\0');
            pixels[44 * 8 + 2] = char(255);
            atlas += pixels;
        }
        for (int i = 0; i < 4; ++i) {
            const uint16_t kern = i == 3 ? 0xffc0 : 0; // AA -1 pixel, others zero.
            atlas += char(kern); atlas += char(kern >> 8);
        }
        std::istringstream input(atlas);
        FontAtlas font(input);
        check(font.advance("AA") == 7 && font.advance("A?") == 8, "Baked kerning and fractional advance units");
        auto glyphs = font.rasterize("AA");
        check(glyphs.height == 64 && glyphs.pixels[44 * glyphs.width + 2] == 255 &&
              glyphs.pixels[44 * glyphs.width + 5] == 255, "Glyph rasterization uses the baked baseline and kerning");
        check(font.advance("\xe2\x80\xa2") == 4, "Unavailable Unicode falls back as one glyph, not three bytes");
        rejected = false;
        try { std::istringstream short_input(atlas.substr(0, atlas.size() - 1)); FontAtlas invalid(short_input); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "Reject truncated kerning data");
        if (argc == 3 && std::string(argv[1]) == "--font") {
            FontAtlas real_font(argv[2]);
            check(real_font.advance("AV") < real_font.advance("A") + real_font.advance("V"), "Real Chakra Petch kerning");
            check(real_font.rasterize("VRX 58C \xe2\x80\xa2 11.5V").pixels.size() < 128 * 1024, "Real HUD text stays bounded");
        }
        {
            Model native; Controller c(native);
            auto& t = native.backend.vrx;
            const uint64_t now = 10000000;
            t.stamp_us = now; t.connected = true; t.mcs = 5; t.frequency_mhz = 5800;
            native.backend.media.video_monitored = true;
            check(native.show_status(now) && native.status(now) == "RF CONNECTED / WAITING FOR VIDEO",
                  "RF association must not imply decoded video");
            auto data = c.hud_data(now);
            for (const auto& metric : data.metrics) {
                if (metric.id == HudMetricId::CpuTemperature || metric.id == HudMetricId::RfTemperatureRaw ||
                    metric.id == HudMetricId::ReceiverVoltage || metric.id == HudMetricId::QualityRaw)
                    check(!metric.valid, "Missing native sensors must remain unavailable");
            }
            native.backend.media.video_stamp_us = now;
            check(!native.show_status(now), "A decoded frame clears waiting-for-video");
            t.stamp_us = 0; t.connected = false;
            check(!native.show_status(now) && native.status(now) == "RECEIVING VIDEO",
                  "Fresh decoded video must not be hidden by missing RF telemetry");
            check(!native.backend.vrx.link_valid(now), "Video must not invent an RF connection");
            t.stamp_us = now + 2000000; t.connected = true;
            check(native.show_status(now + 2000000), "Video silence reopens status even with fresh RF stats");
        }
        {
            auto prefs=model.preferences(); prefs.link_edge_warning=false;
            model.set_preferences(prefs);
            check(!model.preferences().link_edge_warning,"Edge warning preference round trip");
        }
        {
            LinkWarning warning;
            warning.update(1000000,true,0,true,0,false);
            check(warning.level()==0,"No stall before first frame on connection");
            warning.update(1100000,true,1,true,1100000,false);
            check(warning.level()==1,"Amber range");
            warning.update(1200000,true,2,true,1200000,false);
            check(warning.level()==2,"Red range immediately");
            warning.update(1300000,true,0,true,1300000,false);
            warning.update(3299999,true,0,true,3299999,false);
            check(warning.level()==2,"Recovery holds two seconds");
            warning.update(3300000,true,0,true,3300000,false);
            check(warning.level()==0,"Stable recovery clears warning");
            warning.update(3700000,true,0,true,3300000,false);
            check(warning.level()==0,"400ms boundary still healthy");
            warning.update(3700001,true,0,true,3300000,false);
            check(warning.level()==2,"Video stall overrides a good range");
            warning.update(3800000,false,0,true,3300000,false);
            warning.update(5900000,false,0,true,3300000,false);
            check(warning.level()==0,"Disconnect clears held warning");
            warning.update(6000000,true,0,true,3300000,false);
            check(warning.level()==0,"Old connection frame cannot trigger a new stall");
            warning.update(6100000,true,0,true,6000000,true);
            warning.update(7200000,true,0,true,6000000,true);
            check(warning.level()==0,"Paused gallery is not a live video stall");
        }
        {
            // The VRX Pro's range blocks (system/gnd/link_range.hpp), by the
            // radio's MCS index (MCS + 2) and bandwidth step (4 = 20 MHz).
            const auto full=gnd::link_range(true,8,4,501,26);
            check(full.blocks==8 && full.warning()==0,"20 MHz MCS 6 at full AUTO power is 8 blocks, fine");
            check(gnd::link_range(true,7,4,501,26).warning()==1,"20 MHz MCS 5 is amber");
            check(gnd::link_range(true,5,3,501,26).warning()==2,"10 MHz MCS 3 is red");
            check(gnd::link_range(true,8,4,500,0).blocks==9,"A fixed power has no power lever");
            check(gnd::link_range(true,0,4,501,26).blocks<0 && gnd::link_range(false,8,4,501,26).blocks<0,
                  "No MCS reading or no link: no verdict");
            Model m; const uint64_t now=10000000;
            m.backend.vrx.connected=true; m.backend.vrx.stamp_us=now; m.backend.vrx.mcs=8;
            m.backend.link.bandwidth_step=4; m.backend.link.air_power_mw=501; m.backend.link.air_live_dbm=26;
            check(m.link_range(now).blocks==8,"Model reads the range from the backend");
            check(m.focus_row("screen_mode",now) && m.side==0 && m.rows(now)[m.selected].id=="screen_mode",
                  "Focus finds the Screen Mode row wherever it is");
            check(!m.focus_row("no such row",now),"Focus on a missing row fails");
        }
        {
            Model m; Controller c(m); auto& b=m.backend;
            uint64_t now=10000000;
            // MCS index 5 (MCS 3) at 20 MHz on 100 mW AUTO: 6 of 12 range blocks, amber.
            b.vrx.connected=true; b.vrx.mcs=5; b.vrx.stamp_us=now; b.vrx.frequency_mhz=5839;
            b.link={1234,4,101,4.4f,9.7f,true,true,88};
            b.video={now,1920,1080,59,42};
            b.media.recording=true; b.media.recording_id="first";
            b.flight.stamp_us=now; b.flight.arm=1; b.flight.have_timer=true; b.flight.timer_s=25;
            c.tick(now); c.tick(now+500000);
            auto h=c.hud_data(now+500000);
            check(h.panels[0].recording=="REC 0:00" && h.panels[0].state=="ARMED","Recording starts immediately, arm transition held");
            check(h.panels[0].upper_label=="CELL","Cell gauge independent of primary voltage preference");
            check(h.panels[1].secondary=="1234" && h.panels[0].detail1.find("1.23km")!=std::string::npos,"Calibrated distance in both wings");
            check(h.panels[1].state=="STANDBY" && h.panels[1].detail1.find("5839@20MHz")!=std::string::npos &&
                  h.panels[1].detail1.find("100mW AUTO")!=std::string::npos,"Native link details populated");
            check(h.panels[1].detail1_alert==2 && h.panels[1].lower_alert==1,"Temperature and link alerts independent");
            check(h.panels[1].detail2.find("4.4/9.7Mbps")!=std::string::npos && h.panels[1].detail2.find("42ms • 1080p60")!=std::string::npos,"Measured stream statistics and snapped camera mode");
            m.show_hud=false; m.show_details=false;
            c.tick(now+65000000);
            check(!c.hud_data(now+65000000).enabled && c.recording_text(now+65000000)=="REC 1:05","Recording timer survives hidden HUD/details and stale RF");
            b.media.recording=false; c.tick(now+66000000);
            check(c.recording_text(now+66000000).empty(),"Recording stop clears indicator");
            b.media.recording=true; b.media.recording_id="second"; c.tick(now+67000000);
            check(c.recording_text(now+67000000)=="REC 0:00","New recording resets timer");
            m.show_hud=true;
            h=c.hud_data(now+67000000);
            check(h.panels[1].secondary.empty() && h.panels[1].detail1.find("--mW")!=std::string::npos && h.panels[1].detail2.find("--ms")!=std::string::npos,"Expired readings are not displayed as current");
        }
        std::puts("PASS: telemetry freshness, shared canopy/GLES geometry, projective rasterization, baked fonts and menus");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}

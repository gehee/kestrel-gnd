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
        check(model.stage == MenuStage::Browse && model.tab == 1, "Open the goggle HUD blade");
        controller.key(Key::Enter); controller.key(Key::Down);
        check(model.stage == MenuStage::Edit && model.theme == 1, "Preview local theme in an edit transaction");
        controller.key(Key::Back);
        check(model.stage == MenuStage::Browse && model.theme == 0, "Back cancels and restores the previous theme");
        controller.key(Key::Enter); controller.key(Key::Down); controller.key(Key::Enter);
        check(model.stage == MenuStage::Browse && model.theme == 1, "OK commits the preview");
        for (int i = 0; i < 40; ++i) controller.key(Key::Down);
        check(model.selected < int(model.rows(1000000).size()), "Dynamic row bounds");
        controller.key(Key::Back); check(model.stage == MenuStage::Pick && model.menu_open, "Back folds the blade first");
        controller.key(Key::Back); check(!model.menu_open, "Back then closes the canopy");
        controller.key(Key::Enter); controller.key(Key::Enter); controller.key(Key::Right);
        check(model.tab == 3, "Sections stay within their selected side");
        controller.key(Key::Enter); controller.key(Key::Down); check(model.fps == 26, "UI cadence preview");
        model.tab = 2; controller.key(Key::Enter);
        check(model.fps == 25 && model.stage == MenuStage::Browse, "Changing rows during editing cancels; it cannot activate another action");
        Model failure;
        Controller failure_controller(failure);
        failure_controller.save_preferences = [](const UiPreferences&) { throw std::runtime_error("read-only config"); };
        failure_controller.key(Key::Enter); failure_controller.key(Key::Enter); failure_controller.key(Key::Enter); failure_controller.key(Key::Down); failure_controller.key(Key::Enter);
        check(failure.theme == 0 && failure.feedback.find("Save failed") != std::string::npos, "Failed persistence rolls back the preview");
        Key parsed;
        check(parse_key(" OK\n", parsed) && parsed == Key::Enter && !parse_key("bind", parsed), "Named controls are restricted to UI keys");
        model.key_repeat = false; const auto keys_before = model.key_count;
        controller.key(Key::Up, true); check(model.key_count == keys_before, "Disabled repeats do not move the menu");
        model.key_repeat = true; controller.key(Key::Menu, true); check(model.key_count == keys_before, "Menu keys never auto-repeat");
        controller.set_scene(SceneMode::GalleryBrowse);
        check(!model.hud_visible() && !controller.hud_data(2300000).enabled, "Gallery browsing hides the passive HUD");
        controller.set_scene(SceneMode::GalleryPlayback);
        check(!model.hud_visible(), "Gallery playback hides the passive HUD");
        controller.key(Key::Back);
        check(model.scene_mode() == SceneMode::Live && model.hud_visible(), "Back from gallery restores live HUD visibility");
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
        store_preferences(settings_path, {2, 30, false, false});
        const auto loaded = load_preferences(settings_path);
        check(loaded.theme == 2 && loaded.fps == 30 && !loaded.details && !loaded.repeat, "Local preferences round trip");
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
        std::puts("PASS: stock protocol, freshness, shared canopy/GLES geometry, projective rasterization, baked fonts and menus");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}

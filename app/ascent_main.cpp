// Native Ascent composition root: existing radio, software HUD, platform media.
#include "base/ltrace.hpp"
#include "platform/software.hpp"
#include "platform/ascent_panel.hpp"
#include "kestrel_gnd_config.h"
#include "radio/ar8030_link.hpp"
#include "hud/lvgl/display.hpp"
#include "hud/lvgl/input.hpp"
#include "hud/lvgl/preferences.hpp"
#include "hud/lvgl/view.hpp"
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
volatile sig_atomic_t interrupted = 0, menu_requested = 0;
void signal_handler(int signal) {
    if (signal == SIGUSR1)
        menu_requested = 1;
    else
        interrupted = 1;
}
struct Options {
    platform::SoftwareSetup platform;
    std::string trace_dir;
    std::string assets = "/usr/share/kestrel/ui", input, control, preferences, command, screenshot;
    bool radio = true, menu = false, vector = false, profile = false;
    unsigned seconds = 0;
    Ar8030Config link;
};
Options options(int argc, char **argv) {
    Options o;
    o.link.native_sdio = true;
    o.link.radio_fw = 1;
    o.link.decode_enabled = true;
    o.link.stream_decode = false;
    o.link.replay_stock_rf = 0;
    o.link.tx_power_dbm = 14;
    o.link.tx_power_mw = o.link.tx_power_want_mw = 25;
    o.link.mode_w = 1920;
    o.link.mode_h = 1080;
    o.link.mode_fps = 60;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i == argc)
                throw std::invalid_argument("missing value for " + arg);
            return argv[i];
        };
        if (arg == "--assets")
            o.assets = value();
        else if (arg == "--drm")
            o.platform.display_device = value();
        else if (arg == "--decoder")
            o.platform.decoder_device = value();
        else if (arg == "--input")
            o.input = value();
        else if (arg == "--control")
            o.control = value();
        else if (arg == "--screenshot")
            o.screenshot = value();
        else if (arg == "--preferences")
            o.preferences = value();
        else if (arg == "--send-key")
            o.command = value();
        else if (arg == "--force-display")
            o.platform.force_display = true;
        else if (arg == "--no-radio")
            o.radio = false;
        else if (arg == "--show-menu")
            o.menu = true;
        else if (arg == "--trace-dir")
            o.trace_dir = value();
        else if (arg == "--profile")
            o.profile = true;
        else if (arg == "--vector")
            o.vector = true;
        else if (arg == "--replay")
            o.link.replay_path = value();
        else if (arg == "--seconds") {
            auto s = value();
            size_t end = 0;
            auto n = std::stoul(s, &end);
            if (end != s.size() || n > 86400)
                throw std::invalid_argument("seconds must be 0..86400");
            o.seconds = n;
        } else if (arg == "--peer") {
            auto s = value();
            unsigned a, b, c, d;
            int used = 0;
            if (sscanf(s.c_str(), "%2x:%2x:%2x:%2x%n", &a, &b, &c, &d, &used) != 4 || used != int(s.size()))
                throw std::invalid_argument("peer must be a four-byte radio MAC: AA:BB:CC:DD");
            o.link.peer = std::array<uint8_t, 4>{uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d)};
        } else if (arg == "--help") {
            puts("Ascent VRX receiver (native fpvOS, 1920x1080@60)\n"
                 "  --peer AA:BB:CC:DD   Air unit radio MAC, for this session\n"
                 "  --assets DIR        Generated LVGL assets\n"
                 "  --force-display     Use fixed HDMI mode when detection reports disconnected\n"
                 "  --input /dev/input/eventN  Optional evdev buttons\n"
                 "  --control PATH      Optional local UI control socket\n"
                 "  --send-key KEY      Send menu/up/down/left/right/enter/back/quit to that socket\n"
                 "  --preferences FILE  Save visual preferences (optional)\n"
                 "  --show-menu --vector --no-radio --seconds N\n"
                 "  --profile           Log UI and display timing every five seconds\n"
                 "  --trace-dir DIR     Watch DIR/ltrace.req; write frame timing to DIR/ltrace.bin\n"
                 "  --drm DEVICE --decoder DEVICE --replay FILE");
            exit(0);
        } else
            throw std::invalid_argument("unknown option: " + arg);
    }
    if (o.command.empty() && o.radio && !o.link.peer && o.link.replay_path.empty())
        throw std::invalid_argument("specify --peer for the air unit, or --no-radio for the UI");
    return o;
}

} // namespace
int main(int argc, char **argv) {
    try {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        auto o = options(argc, argv);
        if (!o.command.empty()) {
            if (o.control.empty())
                throw std::invalid_argument("--send-key requires --control");
            puts(kestrel_ui::send_control(o.control, o.command).c_str());
            return 0;
        }
        if (!o.trace_dir.empty())
            ltrace::start(o.trace_dir);
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGUSR1, signal_handler);
        Settings settings;
        radio::Model radio_model(settings);
        bus::Waker updates;
        radio_model.radio().watch(&updates);
        // Resources outlive every thread, including when creating one fails.
        volatile bool stop = false;
        const VideoCodec codec = VideoCodec::H265;
        auto receiver = platform::make_software_receiver(o.platform, codec, &stop);
        Ar8030Link link(radio_model, o.link, settings, codec, receiver.decoder, &stop);
        std::atomic<bool> source_exited{false}, worker_failed{false};
        struct Join {
            volatile bool &stop;
            std::thread display, decoder, radio;
            ~Join() { finish(); }
            void finish() {
                stop = true;
                if (radio.joinable())
                    radio.join();
                if (decoder.joinable())
                    decoder.join();
                if (display.joinable())
                    display.join();
            }
        } workers{stop, {}, {}, {}};
        workers.display = std::thread([&] { receiver.screen->run(); });
        if (o.radio) {
            workers.decoder = std::thread([&] { receiver.decoder->run_frame(); });
            workers.radio = std::thread([&] {
                try {
                    link.run();
                } catch (const std::exception &e) {
                    fprintf(stderr, "radio: %s\n", e.what());
                    worker_failed = true;
                }
                source_exited = true;
            });
        }
        lv_init();
        lv_tick_set_cb([] { return uint32_t(kestrel_ui::monotonic_us() / 1000); });
        kestrel_ui::Display display(1920, 1080, "");
        kestrel_ui::Model model;
        model.build = kestrel_gnd_build_time;
        model.version = std::string(kestrel_gnd_version) + " (" + kestrel_gnd_git_hash + ")";
        model.decoder = "V4L2 / Wave5";
        model.screen_mode = "1920x1080 @ 60 Hz";
        model.backend.media.video_monitored = o.radio;
        model.backend.service.adapter = "fpvOS native Ascent";
        model.backend.service.hardware_delegated = false;
        model.backend.vrx.freshness_us = 6000000; // native full radio poll is every three seconds
        model.set_preferences(kestrel_ui::load_preferences(o.preferences));
        kestrel_ui::Controller controller(model);
        if (!o.preferences.empty())
            controller.save_preferences = [&](const auto &p) {
                kestrel_ui::store_preferences(o.preferences, p);
            };
        kestrel_ui::Input input(o.input, o.control, false);
        platform::AscentPanel panel;
        model.input_sources = input.sources() + panel.sources();
        model.control_path = o.control;
        model.hud_style = o.vector ? "vector" : "canopy";
        kestrel_ui::InlineHud hud(1920, 1080, o.assets,
                                  o.vector ? kestrel_ui::HudStyle::Vector : kestrel_ui::HudStyle::Canopy);
        kestrel_ui::View view(controller, 1920, 1080, o.assets, &hud);
        if (o.menu)
            controller.open_menu();
        const auto start = kestrel_ui::monotonic_us();
        uint64_t drawn = 0;
        int previous_bind = gnd::BIND_IDLE;
        uint64_t profile_start = start, profile_loops = 0, profile_drawn = 0, profile_presented = 0;
        uint64_t render_us = 0, overlay_us = 0, longest_loop_us = 0;
        uint64_t longest_render_us = 0, longest_overlay_us = 0;
        while (!interrupted && !model.quit && !source_exited && !receiver.screen->failed() &&
               !receiver.decoder->failed()) {
            const auto now = kestrel_ui::monotonic_us();
            if (o.seconds && now - start >= uint64_t(o.seconds) * 1000000)
                break;
            if (menu_requested) {
                menu_requested = 0;
                controller.key(kestrel_ui::Key::Menu);
            }
            model.backend.media.video_stamp_us = receiver.decoder->last_frame_us();
            const auto state = radio_model.radio().state();
            auto &telemetry = model.backend.vrx;
            telemetry.stamp_us = state.link.last_update_ms * 1000;
            telemetry.connected = state.link.active && state.link.state == gnd::kLinkUp;
            telemetry.mcs = telemetry.connected ? state.link.mcs : -1;
            telemetry.frequency_mhz = state.link.tx_freq;
            // Native ranging is metres; the legacy raw field is not reused.
            telemetry.distance_raw = kestrel_ui::Telemetry::unavailable;
            model.backend.service.connected = o.radio;
            model.backend.service.provider_connected = telemetry.fresh(now);
            input.poll(controller, now);
            auto notice = [&](const char* text) {
                if (!model.menu_open) controller.open_menu();
                model.feedback = text;
                ++model.menu_version;
                printf("panel: %s\n", text);
            };
            for (const auto& event : panel.take()) {
                if (event.repeat && !model.key_repeat) continue;
                printf("panel: key=%s%s\n", bsp::ascent_panel::name(event.key), event.repeat ? " repeat" : "");
                if (event.key == platform::PanelKey::Rec) {
                    notice("Recording is not available yet");
                    panel.beep(platform::PanelTone::Failure);
                } else if (event.key == platform::PanelKey::Upgrade) {
                    notice("Button-triggered upgrade is not available yet");
                    panel.beep(platform::PanelTone::Failure);
                } else if (event.key == platform::PanelKey::Link) {
                    if (o.radio && o.link.replay_path.empty()) {
                        radio_model.radio().bind();
                        notice("Binding request sent");
                        panel.beep();
                    } else {
                        notice("Binding requires a running radio");
                        panel.beep(platform::PanelTone::Failure);
                    }
                } else {
                    kestrel_ui::Key key;
                    if (kestrel_ui::parse_key(bsp::ascent_panel::name(event.key), key)) {
                        controller.key(key, event.repeat);
                        if (!event.repeat) panel.beep();
                    }
                }
            }
            if (state.bind_state != previous_bind) {
                previous_bind = state.bind_state;
                switch (state.bind_state) {
                case gnd::BIND_RUNNING:
                    notice("Binding: press LINK again to cancel");
                    panel.beep(platform::PanelTone::Notice); break;
                case gnd::BIND_OK:
                    notice("Binding succeeded");
                    panel.beep(platform::PanelTone::Success); break;
                case gnd::BIND_FAILED:
                    notice("Binding failed");
                    panel.beep(platform::PanelTone::Failure); break;
                case gnd::BIND_IDLE: notice("Binding ended"); break;
                default: break;
                }
            }
            controller.tick(now);
            const auto render_start = kestrel_ui::monotonic_us();
            view.update(now);
            lv_timer_handler();
            lv_refr_now(display.lv_display());
            const auto overlay_start = kestrel_ui::monotonic_us();
            if (display.presentations != drawn) {
                drawn = display.presentations;
                receiver.screen->overlay(display.pixels().data(), 1920, 1080);
            }
            const auto rendered = kestrel_ui::monotonic_us();
            // FPS is a frame budget, not an extra delay after rendering.
            const uint64_t frame_us = 1000000 / std::max(1, model.fps);
            if (rendered - now < frame_us)
                usleep(frame_us - (rendered - now));
            if (o.profile) {
                const auto ended = kestrel_ui::monotonic_us();
                ++profile_loops;
                render_us += overlay_start - render_start;
                overlay_us += rendered - overlay_start;
                longest_loop_us = std::max(longest_loop_us, ended - now);
                longest_render_us = std::max(longest_render_us, overlay_start - render_start);
                longest_overlay_us = std::max(longest_overlay_us, rendered - overlay_start);
                if (ended - profile_start >= 5000000) {
                    const double seconds = double(ended - profile_start) / 1000000;
                    const auto presented = receiver.screen->presented();
                    printf("profile: target=%d loops=%.2f/s UI=%.2f fps HDMI_updates=%.2f/s "
                           "render=%.1fms overlay=%.1fms max_loop=%.1fms max_render=%.1fms max_overlay=%.1fms\n",
                           model.fps, profile_loops / seconds, (drawn - profile_drawn) / seconds,
                           (presented - profile_presented) / seconds,
                           render_us / (1000.0 * profile_loops), overlay_us / (1000.0 * profile_loops),
                           longest_loop_us / 1000.0, longest_render_us / 1000.0, longest_overlay_us / 1000.0);
                    profile_start = ended; profile_drawn = drawn; profile_presented = presented;
                    profile_loops = render_us = overlay_us = longest_loop_us = 0;
                    longest_render_us = longest_overlay_us = 0;
                }
            }
        }
        if (!o.screenshot.empty())
            display.dump(o.screenshot);
        const bool source_failed = source_exited.load();
        workers.finish();
        printf("ascent: HDMI presentations=%llu UI presentations=%llu\n",
               (unsigned long long)receiver.screen->presented(), (unsigned long long)display.presentations);
        return source_failed || worker_failed || receiver.screen->failed() || receiver.decoder->failed() ? 1
                                                                                                         : 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "ascent: %s\n", e.what());
        return 1;
    }
}

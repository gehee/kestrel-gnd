// Native Ascent composition root: existing radio, software HUD, platform media.
#include "platform/software.hpp"
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
    std::string assets = "/usr/share/kestrel/ui", input, control, preferences, command, screenshot;
    bool radio = true, menu = false, vector = false;
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
        model.input_sources = input.sources();
        model.control_path = o.control;
        model.hud_style = o.vector ? "vector" : "canopy";
        kestrel_ui::InlineHud hud(1920, 1080, o.assets,
                                  o.vector ? kestrel_ui::HudStyle::Vector : kestrel_ui::HudStyle::Canopy);
        kestrel_ui::View view(controller, 1920, 1080, o.assets, &hud);
        if (o.menu)
            controller.open_menu();
        const auto start = kestrel_ui::monotonic_us();
        uint64_t drawn = 0;
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
            controller.tick(now);
            view.update(now);
            lv_timer_handler();
            lv_refr_now(display.lv_display());
            if (display.presentations != drawn) {
                drawn = display.presentations;
                receiver.screen->overlay(
                    std::make_shared<hal::I420Overlay>(1920, 1080, display.pixels().data()));
            }
            usleep(1000000 / std::max(1, model.fps));
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

// Deterministic offscreen states for comparison with tools/test/hud_shot.cpp.
#include "display.hpp"
#include "view.hpp"
#include <filesystem>
#include <cstdio>

int main(int argc, char** argv) {
    using namespace kestrel_ui;
    if (argc != 3 && argc != 4) { std::fprintf(stderr, "usage: %s ASSETS OUTDIR [720|1080]\n", argv[0]); return 2; }
    try {
        const int h=argc==4?std::stoi(argv[3]):1080,w=h==720?1280:1920;
        if(h!=720 && h!=1080)throw std::runtime_error("Unsupported UI profile");
        std::filesystem::create_directories(argv[2]); lv_init();
        {
            Display display(w, h, ""); Model model; Controller controller(model);
            View view(controller, w, h, argv[1]); uint64_t now = monotonic_us();
            auto frame = [&] {
                now += 40000;
                if (model.backend.vrx.connected) model.backend.vrx.stamp_us = now;
                controller.tick(now); view.update(now); lv_tick_inc(40); lv_refr_now(display.lv_display());
            };
            auto shot = [&](const std::string& name) {
                for (int i = 0; i < 16; ++i) frame();
                display.dump(std::string(argv[2]) + "/" + name + ".pam");
            };
            shot("idle"); controller.open_menu(); shot("picker");
            controller.key(Key::Enter); shot("ground_hud");
            controller.key(Key::Right); shot("ground_display");
            controller.key(Key::Enter); controller.key(Key::Down); shot("theme_edit");
            controller.key(Key::Up); controller.key(Key::Enter);
            controller.key(Key::Right); shot("ground_dvr"); controller.key(Key::Right); shot("ground_system");
            controller.key(Key::Back); controller.key(Key::Right); controller.key(Key::Enter); shot("air_unlinked");
            model.backend.vrx.connected = true; model.backend.vrx.mcs = 8;
            model.backend.media.video_monitored = false;
            for (int tab : {0, 1, 7, 6}) {
                model.tab = tab; model.selected = model.first_row(now); ++model.menu_version;
                shot(std::string("air_") + Model::tab_name(tab));
            }
        }
        lv_deinit(); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}

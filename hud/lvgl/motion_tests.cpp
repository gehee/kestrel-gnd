#include "display.hpp"
#include "view.hpp"
#include "controller.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("Usage: motion-tests ASSETS HEIGHT");
        const int height=std::stoi(argv[2]),width=height*16/9;
        check(height==720 || height==1080,"Unsupported test resolution");
        lv_init();
        {
            kestrel_ui::Display display(width,height,"");
            kestrel_ui::Model model;
            kestrel_ui::Controller controller(model);
            kestrel_ui::View view(controller,width,height,argv[1]);
            model.clock=0;model.debug=false;model.bf_osd=false;model.link_edge_warning=false;
            model.dynamic=2;model.show_hud=true;
            model.backend.vrx.connected=true;model.backend.vrx.mcs=5;
            model.backend.vrx.voltage_mv=12000;
            uint64_t now=1000000;
            auto draw=[&] {view.update(now);lv_refr_now(display.lv_display());};
            auto settle=[&] {
                for(int i=0;i<120;++i) {
                    now+=16667;model.backend.vrx.stamp_us=now;
                    controller.tick(now);draw();
                }
            };
            auto full_matches=[&] {
                const auto incremental=display.pixels();
                lv_obj_invalidate(lv_screen_active());lv_refr_now(display.lv_display());
                check(std::equal(incremental.begin(),incremental.end(),display.pixels().begin(),
                    [](uint32_t a,uint32_t b){return a==b || ((a|b)>>24)==0;}),
                    "Translation left stale pixels compared with full redraw");
            };
            settle();
            for(bool replay:{false,true}) {
                if(replay) {
                    model.backend.playback_hud=std::make_shared<kestrel_ui::HudData>(controller.hud_data(now,true));
                    model.playback_hud_enabled=true;model.backend.playback_paused=true;
                    controller.set_scene(kestrel_ui::SceneMode::GalleryPlayback);
                    settle();
                }
                auto& dx=replay?model.backend.playback_dx:model.backend.hud_dx;
                auto& dy=replay?model.backend.playback_dy:model.backend.hud_dy;
                auto& bank=replay?model.backend.playback_bank:model.backend.hud_bank;
                dx=dy=bank=0;model.dynamic=2;draw();
                const auto centered=display.pixels();
                dx=.12f;dy=.06f;draw();
                check(display.pixels()!=centered,"HUD offsets did not move visible pixels");
                full_matches();
                const auto moved=display.pixels();
                const auto presentations=display.presentations;
                for(float value:{.5f,-.5f,0.f}) {
                    bank=value;draw();
                    check(display.pixels()==moved,"Bank telemetry rotated the software HUD");
                    check(display.presentations==presentations,"Bank-only updates caused redundant redraws");
                }
                // A held replay pose / unchanged live pose does not consume redraws.
                for(int i=0;i<10;++i)draw();
                check(display.presentations==presentations,"Held pose caused redundant redraws");
                dx=-.08f;dy=-.04f;draw();full_matches();
                model.dynamic=0;draw();
                check(display.pixels()==centered,"Dynamic HUD OFF did not center the panels");
                full_matches();
                // Hidden HUD and scene changes also need to erase translated pixels.
                model.dynamic=2;dx=.1f;dy=.02f;draw();
                model.show_hud=false;draw();full_matches();
                model.show_hud=true;draw();full_matches();
            }
        }
        lv_deinit();
        std::printf("PASS: %dp live/replay translation, bank ignored, held pose and damage\n",height);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"motion tests: %s\n",e.what());return 1;
    }
}

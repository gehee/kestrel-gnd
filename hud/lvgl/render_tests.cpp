#include "display.hpp"
#include "view.hpp"
#include "controller.hpp"
#include "hud/hud_theme.hpp"
#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: kestrel-ui-render-tests GENERATED_1080P_ASSETS");
        lv_init();
        {
            kestrel_ui::Display display(1920, 1080, "");
            kestrel_ui::Model model;
            kestrel_ui::Controller controller(model);
            kestrel_ui::View view(controller, 1920, 1080, argv[1]);
            uint64_t now = 1000000;
            for (int frame = 0; frame < 240; ++frame) {
                now += 40000;
                auto& t = model.backend.vrx;
                t.stamp_us = now; t.connected = true; t.mcs = 1 + frame % 12;
                t.cpu_temperature_x100 = (30 + frame) * 100;
                t.voltage_mv = 11000 + frame * 100; t.frequency_raw = 5800;
                t.status_frames = frame + 1;
                auto& flight = model.backend.flight;
                flight.stamp_us = now;
                flight.have_cell_v = flight.have_amps = flight.have_speed = flight.have_lq = flight.have_timer = flight.have_mode = true;
                flight.cell_v = 4.2f - (frame % 80) * 0.005f;
                flight.amps = 5 + frame % 10; flight.speed_kmh = 50 + frame % 20; flight.lq_pct = 94;
                flight.arm = (frame / 30) % 2; flight.timer_s = frame;
                std::snprintf(flight.mode, sizeof(flight.mode), "ACRO");
                model.backend.flight_version = frame + 1;
                model.menu_open = frame % 40 < 20;
                model.tab = (frame / 20) % 3;
                model.side = model.tab == 0 ? 1 : 0;
                model.stage = frame % 40 < 6 ? kestrel_ui::MenuStage::Pick : kestrel_ui::MenuStage::Browse;
                model.selected = frame % int(model.rows(now).size());
                model.theme = (frame / 30) % hud_theme_count();
                controller.tick(now); view.update(now);
                lv_tick_inc(40);
                lv_refr_now(display.lv_display());
                if (view.text_cache_bytes() > 1500 * 1024) throw std::runtime_error("Projected text cache exceeded its frame budget");
            }
            model.menu_open = true; model.side = 0; model.tab = 3; model.selected = 0;
            model.stage = kestrel_ui::MenuStage::Browse;
            controller.key(kestrel_ui::Key::Enter);
            for (int frame = 0; frame < 24; ++frame) {
                now += 40000; model.backend.vrx.stamp_us = now; model.backend.flight.stamp_us = now;
                if (frame % 4 == 0) controller.key(kestrel_ui::Key::Down);
                controller.tick(now); view.update(now); lv_tick_inc(40); lv_refr_now(display.lv_display());
            }
            controller.key(kestrel_ui::Key::Back);
            controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            if (view.text_rasterizations() < 240) throw std::runtime_error("Test did not exercise changing projected text / eviction");
            const auto warm = view.text_rasterizations();
            for (int i = 0; i < 4; ++i) {
                controller.tick(now); view.update(now);
                lv_obj_invalidate(lv_screen_active());
                lv_refr_now(display.lv_display());
            }
            if (view.text_rasterizations() != warm) throw std::runtime_error("Unchanged text was rasterized again");
            // Hidden picker prewarming must capture real pixels; both sides
            // must reveal the same opaque ground at the correct screen edge.
            auto check_incremental = [&]() {
                const auto incremental = display.pixels();
                lv_obj_invalidate(lv_screen_active()); lv_refr_now(display.lv_display());
                const auto first = std::mismatch(incremental.begin(), incremental.end(), display.pixels().begin(),
                    [](uint32_t a, uint32_t b) { return a == b || ((a | b) >> 24) == 0; }).first - incremental.begin();
                if (size_t(first) != incremental.size()) {
                    std::fprintf(stderr, "redraw difference side=%d stage=%d animating=%d xy=%ld,%ld before=%08x after=%08x\n",
                                 model.side, int(model.stage), view.animating(), long(first % 1920), long(first / 1920),
                                 incremental[first], display.pixels()[first]);
                    throw std::runtime_error("Incremental menu reveal differs from a complete redraw");
                }
            };
            for (int side : {0, 1}) {
                model.menu_open = false; model.stage = kestrel_ui::MenuStage::Pick;
                for (int i = 0; i < 8; ++i) {
                    now += 40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                }
                model.menu_open = true; model.side = side; model.stage = kestrel_ui::MenuStage::Pick;
                now += 40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                const auto captured = view.menu_snapshots();
                if (!captured) throw std::runtime_error("Hidden picker did not prepare a menu snapshot");
                controller.key(kestrel_ui::Key::Enter);
                now += 40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                const auto prepared = view.menu_snapshots();
                now += 40000; ++model.backend.vrx.cpu_temperature_x100;
                controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                check_incremental();
                if (view.menu_snapshots() != prepared) throw std::runtime_error("Moving blade rebuilt its snapshot for telemetry");
                // Original panels are translucent and grow from the folded
                // canopy bounds. Check compositing, not an opaque flat color.
                const int x = side ? 1915 : 4;
                if (!(display.pixels()[size_t(900) * 1920 + x] >> 24))
                    throw std::runtime_error("Blade did not cover its screen edge");
                for (int i = 0; i < 8; ++i) {
                    now += 40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                    check_incremental();
                }
                const auto closing = view.menu_snapshots();
                controller.key(kestrel_ui::Key::Back);
                now += 40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                if (view.menu_snapshots() != closing) throw std::runtime_error("Closing blade replaced its outgoing snapshot");
                check_incremental();
                for (int i = 0; i < 16; ++i) {
                    now += 20000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                    check_incremental();
                }
                // Link-loss animation may still be running independently.
            }
            // A high-rate MSP feed must not rebuild a large SERIAL snapshot at
            // every packet. Passive values are sampled; keys remain immediate.
            model.menu_open = true; model.side = 1; model.tab = 7; model.stage = kestrel_ui::MenuStage::Browse;
            for (int i = 0; i < 16; ++i) {
                now += 20000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            }
            const auto serial_warm = view.menu_snapshots();
            for (int i = 0; i < 50; ++i) {
                now += 20000; model.backend.serial.stamp_us = now; ++model.backend.serial.frames; model.backend.serial.bytes += 34;
                controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            }
            if (view.menu_snapshots() - serial_warm > 5 || view.menu_snapshots() == serial_warm)
                throw std::runtime_error("SERIAL counters did not refresh at a bounded rate");
            const auto before_key = view.menu_snapshots();
            controller.key(kestrel_ui::Key::Down);
            now += 20000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            if (view.menu_snapshots() == before_key) throw std::runtime_error("Serial snapshot throttling delayed a navigation key");
            // Exercise the actual right-wing gauge with RSSI-only telemetry.
            // Keep its source/label fixed so a pixel change proves that the
            // filled rung changed, rather than just the caption.
            model.menu_open = false; model.stage = kestrel_ui::MenuStage::Pick;
            model.backend.flight.have_lq = false; model.backend.flight.have_rssi = true; model.backend.flight.rssi_pct = 0;
            for (int i = 0; i < 30; ++i) {
                now += 20000; model.backend.vrx.stamp_us = now; model.backend.flight.stamp_us = now;
                controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            }
            // Take a settled reference, independently of the preceding link
            // acquisition/menu damage, before comparing the RSSI rung itself.
            lv_obj_invalidate(lv_screen_active()); lv_refr_now(display.lv_display());
            kestrel_ui::FontAtlas font(std::string(argv[1]) + "/canopy-font.kuf");
            const auto frame = canopy::screen_frame(1920, 1080);
            const float label = std::max(font.advance("RC RSSI"), font.advance("VIDEO")) *
                canopy::kTextLabel / (canopy::kTextCellHeight * frame.UX);
            const float start = 14 + label + 12;
            const float pitch = (271 - start) / (20 * (1 - 0.32f / 2));
            float cursor = start;
            for (int i = 0; i < 2; ++i) cursor += pitch * (1 - 0.32f * i / 19);
            const float factor = 1 - 0.32f * 2 / 19;
            const auto point = canopy::project(1920, 1080, false, cursor + pitch * factor * 5 / 13, 525 - 4.5f * factor);
            const size_t pixel = size_t(std::lround(point.y)) * 1920 + size_t(std::lround(point.x));
            const auto empty_rung = display.pixels()[pixel];
            model.backend.flight.rssi_pct = 45; ++model.backend.flight_version;
            controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            if (display.pixels()[pixel] == empty_rung) throw std::runtime_error("RSSI-only telemetry did not fill the RC gauge");
            model.backend.flight.rssi_pct = 0; ++model.backend.flight_version;
            controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            if (display.pixels()[pixel] != empty_rung) throw std::runtime_error("RC gauge did not empty when RSSI fell to zero");
            // Static idle assets must survive theme changes and menu motion
            // without stale regions when the live video disappears.
            model.backend.vrx.connected = false;
            model.backend.media.video_monitored = true;
            model.backend.media.video_stamp_us = 0;
            for (int theme = 0; theme < hud_theme_count(); ++theme) {
                model.theme = theme; model.menu_open = true; model.side = theme % 2;
                model.stage = kestrel_ui::MenuStage::Browse;
                for (int i = 0; i < 8; ++i) {
                    now += 40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                    check_incremental();
                }
                model.stage = kestrel_ui::MenuStage::Pick;
                for (int i = 0; i < 8; ++i) {
                    now += 40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
                    check_incremental();
                }
            }
            model.clock=4;
            model.menu_open=true; model.stage=kestrel_ui::MenuStage::Browse; model.side=1; model.tab=1;
            model.backend.channels.readings={{5780,-89,-70,true,"OK"},{5800,-95,-92,true,"OK"},{5820,-75,-74,false,"NOT ALLOWED"}};
            model.backend.channels.current_mhz=5800; model.backend.channels.fresh=true;
            for(bool open:{true,false,true,false}) {
                model.channel_open=open; ++model.menu_version;
                for(int i=0;i<8;++i) {
                    now+=40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display()); check_incremental();
                }
            }
            model.clock=0; model.menu_open=false;
            for(int i=0;i<20;++i) {
                now+=40000; controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            }
            const auto idle_presentations=display.presentations;
            for(int i=0;i<60;++i) {now+=16667;controller.tick(now);view.update(now);lv_refr_now(display.lv_display());}
            if(display.presentations!=idle_presentations) throw std::runtime_error("Static idle rerendered without a content change");
            if(view.betaflight_available()) {
                model.bf_osd=true;model.backend.serial.stamp_us=now;
                const auto empty=display.pixels();
                model.backend.bf_grid[10*53+26]='A';model.backend.bf_grid[10*53+27]=6;
                view.update(now);lv_refr_now(display.lv_display());check_incremental();
                if(display.pixels()==empty) throw std::runtime_error("Betaflight characters did not render");
                model.backend.bf_grid.fill(0);view.update(now);lv_refr_now(display.lv_display());check_incremental();
                if(display.pixels()!=empty) throw std::runtime_error("Betaflight clear left stale glyph pixels");
            }
            model.dynamic=2;model.backend.vrx.connected=true;model.backend.vrx.mcs=5;
            for(int i=0;i<24;++i) {
                now+=16667;model.backend.vrx.stamp_us=now;
                model.backend.hud_dx=.04f*std::sin(i*.3f);model.backend.hud_dy=.02f*std::cos(i*.3f);model.backend.hud_bank=.03f*std::sin(i*.3f);
                controller.tick(now);view.update(now);lv_refr_now(display.lv_display());check_incremental();
            }
            model.dynamic=0;view.update(now);lv_refr_now(display.lv_display());check_incremental();
            const auto unscaled=display.pixels();
            model.ui_scale=15;view.update(now);lv_refr_now(display.lv_display());check_incremental();
            if(display.pixels()==unscaled) throw std::runtime_error("UI scale did not resize HUD text");
            model.ui_scale=10;view.update(now);lv_refr_now(display.lv_display());check_incremental();
            // UI Scaling also changes the open menu's typography and spacing.
            // Measure the selected row, rather than merely checking for any
            // changed pixels (the scale value itself would satisfy that).
            model.theme=0;model.menu_open=true;model.side=0;model.tab=2;model.selected=0;
            model.stage=kestrel_ui::MenuStage::Browse;
            auto menu_row_height=[&](int scale) {
                model.ui_scale=scale;++model.menu_version;
                for(int i=0;i<20;++i) {now+=20000;controller.tick(now);view.update(now);lv_refr_now(display.lv_display());}
                check_incremental();
                int first=1080,last=-1;
                for(int y=100;y<400;++y) {
                    const auto pixel=display.pixels()[y*1920+300];
                    if(((pixel>>16)&255)>150 && ((pixel>>8)&255)>240 && (pixel&255)<25) {
                        first=std::min(first,y);last=y;
                    }
                }
                if(last<first)throw std::runtime_error("Selected menu row was not rendered");
                return last-first+1;
            };
            const auto normal_row=menu_row_height(10),small_row=menu_row_height(8);
            if(small_row>=normal_row || normal_row-small_row<8)throw std::runtime_error("UI Scaling did not resize menu row spacing");
            if(menu_row_height(10)!=normal_row)throw std::runtime_error("Restoring UI Scaling left stale menu geometry");
            // Compare patched row snapshots against a genuinely rebuilt menu,
            // not just a redraw of the same cached image. Theme round-trips
            // force fresh snapshots while preserving layout and content.
            for(int side:{0,1}) for(int tab:(side?std::vector<int>{1,7}:std::vector<int>{2,3})) {
                model.side=side;model.tab=tab;model.selected=0;model.ui_scale=10;
                for(int i=0;i<10;++i){now+=40000;controller.tick(now);view.update(now);lv_refr_now(display.lv_display());}
                for(int row=0;row<int(model.rows(now).size());++row) {
                    model.selected=row;++model.menu_version;
                    view.update(now);lv_refr_now(display.lv_display());check_incremental();
                    const auto patched=display.pixels();
                    model.theme=1;++model.menu_version;view.update(now);lv_refr_now(display.lv_display());
                    model.theme=0;++model.menu_version;view.update(now);lv_refr_now(display.lv_display());
                    if(patched!=display.pixels()) {
                        const auto pos=std::mismatch(patched.begin(),patched.end(),display.pixels().begin()).first-patched.begin();
                        std::fprintf(stderr,"snapshot mismatch side=%d tab=%d row=%d xy=%ld,%ld\n",side,tab,row,long(pos%1920),long(pos/1920));
                        throw std::runtime_error("Patched menu snapshot differs from a fresh capture");
                    }
                }
            }
            const auto settled_menu=display.presentations;
            ++model.menu_version;view.update(now);lv_refr_now(display.lv_display());
            if(display.presentations!=settled_menu)throw std::runtime_error("Unchanged menu version caused a redundant redraw");
            model.backend.recordings={{"recording_20260101_123456.mp4",12345678},{"another.mp4",4000000}};
            for(auto scene:{kestrel_ui::SceneMode::GalleryBrowse,kestrel_ui::SceneMode::GalleryPlayback,kestrel_ui::SceneMode::GalleryBrowse,kestrel_ui::SceneMode::Live}) {
                controller.set_scene(scene);
                for(int i=0;i<20;++i) {
                    now+=20000;controller.tick(now);view.update(now);lv_refr_now(display.lv_display());check_incremental();
                }
            }
            model.menu_open=false; model.show_hud=true; model.show_details=false;
            model.link_edge_warning=false; model.clock=0; model.dynamic=0;
            model.backend.media.recording=false; model.backend.vrx.mcs=8;
            auto draw=[&] {
                now+=20000; model.backend.vrx.stamp_us=now;
                controller.tick(now); view.update(now); lv_refr_now(display.lv_display());
            };
            for(int i=0;i<120;++i) draw();
            auto red_count=[&](int x0,int y0,int x1,int y1) {
                int count=0;
                for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
                    auto pixel=display.pixels()[y*1920+x];
                    const int r=(pixel>>16)&255,g=(pixel>>8)&255;
                    if((pixel>>24)>80 && r>150 && r>g*2) ++count;
                }
                return count;
            };
            const int before=red_count(0,600,700,820);
            model.backend.media.recording=true; model.backend.media.recording_id="pixel-test";
            draw();
            if(red_count(0,600,700,820)<before+30)throw std::runtime_error("Canopy REC badge missing with details hidden");
            check_incremental();
            model.show_hud=false; draw();
            if(red_count(650,0,1270,90)<30)throw std::runtime_error("REC fallback missing with clock/HUD off");
            check_incremental();
            model.backend.media.recording=false; draw();
            if(red_count(650,0,1270,90))throw std::runtime_error("Stopped recording left stale REC badge");
            const auto stable=display.presentations;
            draw();
            if(display.presentations!=stable)throw std::runtime_error("Static hidden HUD caused unnecessary redraw");
            std::printf("PASS: 240 frames, menu motion/tabs/themes, text LRU lifetime, stable-cache reuse, channel/clock damage, static idle; bytes=%zu rasters=%llu\n",
                        view.text_cache_bytes(), (unsigned long long)view.text_rasterizations());
        }
        lv_deinit();
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}

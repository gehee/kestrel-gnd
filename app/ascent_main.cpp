// Native Ascent composition root: existing radio, software HUD, platform media.
#include "base/ltrace.hpp"
#include "display/mode_trial.hpp"
#include "display/settings.hpp"
#include "platform/software.hpp"
#include "platform/ascent_panel.hpp"
#include "kestrel_gnd_config.h"
#include "radio/ar8030_link.hpp"
#include "hud/lvgl/display.hpp"
#include "hud/lvgl/input.hpp"
#include "hud/lvgl/preferences.hpp"
#include "hud/lvgl/receiver_controls.hpp"
#include "ascent_menu.hpp"
#include "dvr/dvr_sink.hpp"
#include "dvr/stream_recorder.hpp"
#include "dvr/settings.hpp"
#include "msp/msp_osd.hpp"
#include "motion/imu_reader.hpp"
#include "hal/source_decoder.hpp"
#include "dvr/stream_player.hpp"
#include "hud/lvgl/view.hpp"
#include "render/software/frame_cadence.hpp"
#include <csignal>
#include <filesystem>
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
    std::string trace_dir, settings_file, recordings;
    std::string assets = "/usr/share/kestrel/ui", input, control, preferences, command, screenshot;
    bool radio = true, menu = false, vector = false, profile = false;
    unsigned seconds = 0;
    bool explicit_mode = false;
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
        else if (arg == "--screen-mode") {
            o.explicit_mode = true;
            const auto mode=value();
            if(mode=="1280x720@60") {o.platform.display_width=1280;o.platform.display_height=720;}
            else if(mode=="1920x1080@60") {o.platform.display_width=1920;o.platform.display_height=1080;}
            else throw std::invalid_argument("Supported screen modes: 1280x720@60, 1920x1080@60");
        }
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
        else if (arg == "--settings")
            o.settings_file = value();
        else if (arg == "--recordings")
            o.recordings = value();
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
            puts("Ascent VRX receiver (native fpvOS, 720p60/1080p60)\n"
                 "  --peer AA:BB:CC:DD   Air unit radio MAC, for this session\n"
                 "  --screen-mode MODE  1280x720@60 or 1920x1080@60\n"
                 "  --assets DIR        Generated LVGL assets\n"
                 "  --force-display     Use fixed HDMI mode when detection reports disconnected\n"
                 "  --input /dev/input/eventN  Optional evdev buttons\n"
                 "  --control PATH      Optional local UI control socket\n"
                 "  --send-key KEY      Send menu/up/down/left/right/enter/back/quit to that socket\n"
                 "  --preferences FILE  Save visual preferences (optional)\n"
                 "  --show-menu --vector --no-radio --seconds N\n"
                 "  --settings FILE     Save receiver and per-air-unit settings\n"
                 "  --recordings DIR    Existing folder for received-stream MP4 recordings\n"
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

int run_session(Options o, Settings& settings, display::ModeTrial& mode) {
    try {
        o.platform.display_height = mode.current;
        o.platform.display_width = mode.current == 720 ? 1280 : 1920;
        zones::load(settings);
        o.link.max_kbps=settings.get(radio::setting::ar8030_max_kbps);
        o.link.max_mcs=settings.get(radio::setting::ar8030_max_mcs);
        o.link.tx_power_mw=o.link.tx_power_want_mw=settings.get(radio::setting::tx_power_mw);
        radio::Model radio_model(settings);
        bus::Waker updates;
        radio_model.radio().watch(&updates);
        bus::Waker fc_updates;
        radio_model.fc().watch(&fc_updates);
        motion::ImuReader imu_reader(radio_model.imu());
        MspOsd flight_controller;
        // Resources outlive every thread, including when creating one fails.
        volatile bool stop = false;
        const auto assets_root = std::filesystem::path(o.assets);
        const int screen_w=o.platform.display_width,screen_h=o.platform.display_height;
        const auto profile=std::filesystem::path(o.assets)/(std::to_string(screen_w)+"x"+std::to_string(screen_h));
        if(std::filesystem::is_directory(profile)) o.assets=profile.string();
        else if(screen_h!=1080)throw std::runtime_error("Install generated 720p UI assets before selecting this mode");
        const VideoCodec codec = VideoCodec::H265;
        auto receiver = platform::make_software_receiver(o.platform, codec, &stop);
        auto source_decoder=std::make_shared<hal::SourceDecoder>([&] {
            return platform::make_software_decoder(o.platform,codec,receiver.screen,&stop);
        },&stop);
        receiver.decoder=source_decoder;
        if(o.recordings.empty()) o.recordings=settings.get(dvr::setting::dvr_dir);
        dvr::StreamRecorder recorder(o.recordings,codec,1920,1080,60);
        dvr::StreamPlayer player(o.recordings,{
            [&](bool playing){receiver.screen->hold_video(playing);source_decoder->playback(playing);},
            [&]{return source_decoder->playback_ready();},
            [&]{return source_decoder->playback_failed() || source_decoder->failed();},
            [&](const std::vector<uint8_t>& data,uint64_t pts,uint8_t nal,bool key) {
                source_decoder->feed_playback(const_cast<uint8_t*>(data.data()),int(data.size()),pts,kestrel_ui::monotonic_us(),nal,key);
            },[&](bool paused){source_decoder->pause_input(paused);}});
        DvrSink recording_sink(codec,recorder);
        Ar8030Link link(radio_model, o.link, settings, codec, receiver.decoder, &stop);
        link.add_video_sink(&recording_sink);
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
        workers.decoder = std::thread([&] { receiver.decoder->run_frame(); });
        if (o.radio) {
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
        struct LvLifetime { ~LvLifetime() { lv_deinit(); } } lv_lifetime;
        lv_tick_set_cb([] { return uint32_t(kestrel_ui::monotonic_us() / 1000); });
        kestrel_ui::Display display(screen_w, screen_h, "");
        kestrel_ui::Model model;
        model.build = kestrel_gnd_build_time;
        model.version = std::string(kestrel_gnd_version) + " (" + kestrel_gnd_git_hash + ")";
        model.decoder = "V4L2 / Wave5";
        model.screen_mode = std::to_string(screen_w)+"x"+std::to_string(screen_h)+" @ 60 Hz";
        model.backend.media.video_monitored = o.radio;
        model.backend.service.adapter = "fpvOS native Ascent";
        model.backend.service.hardware_delegated = false;
        model.backend.vrx.freshness_us = 6000000; // native full radio poll is every three seconds
        model.set_preferences(kestrel_ui::load_preferences(o.preferences));
        kestrel_ui::Controller controller(model);
        ascent::RegionMenu region_menu;
        ascent::ChannelMenu channel_menu(radio_model.radio());
        dvr::StreamPlayer::State playback;
        uint64_t playback_checked=0;
        kestrel_ui::ReceiverControls controls(radio_model.camera(),radio_model.encoder(),radio_model.air_radio(),radio_model.air_unit(),radio_model.radio());
        std::vector<kestrel_ui::ReceiverChoice> screen_choices;
        for(int height:{720,1080}) if(receiver.screen->supports_height(height) &&
            std::filesystem::is_directory(assets_root/(height==720?"1280x720":"1920x1080")))
            screen_choices.push_back({height,height==720?"1280x720 @ 60 Hz":"1920x1080 @ 60 Hz"});
        controller.apply_receiver=[&](const std::string& id,int value) {
            if(id=="screen_mode") {
                if(mode.pending) {
                    mode.keep(); settings.set(display::setting::ascent_screen_height,mode.current); settings.flush();
                    return std::string("Screen mode saved");
                }
                if(recorder.is_recording() || recorder.state().finishing || player.state().busy)
                    throw std::runtime_error("Stop recording or playback before changing screen mode");
                if(std::none_of(screen_choices.begin(),screen_choices.end(),[&](const auto& choice){return choice.value==value;}))
                    throw std::runtime_error("Screen mode or compiled assets unavailable");
                mode.select(value); model.quit=mode.restart;
                return std::string("Testing screen mode");
            }
            if(id=="record") {
                if(!recorder.is_recording() && (player.state().busy || !model.backend.media.video_fresh(kestrel_ui::monotonic_us()) || !recorder.state().storage_ready))
                    throw std::runtime_error("Recording needs live video and writable storage");
                recorder.toggle(); return std::string("Recording change requested");
            }
            if(id.compare(0,5,"play:")==0) {
                const auto rec=recorder.state();
                if(recorder.is_recording() || rec.finishing)throw std::runtime_error("Stop recording and wait for it to finish saving first");
                player.play(id.substr(5));playback=player.state();playback_checked=0;return std::string("Opening recording");
            }
            if(id=="playback_stop") {player.stop();return std::string("Playback stopped");}
            if(id=="playback_pause") {player.pause();return std::string("Playback pause changed");}
            if (region_menu.owns(id)) return region_menu.apply(id,value);
            if (id=="channel") return channel_menu.apply(value,model.backend);
            return controls.apply(id,value,kestrel_ui::monotonic_us());
        };
        if (!o.preferences.empty())
            controller.save_preferences = [&](const auto &p) {
                kestrel_ui::store_preferences(o.preferences, p);
            };
        kestrel_ui::Input input(o.input, o.control, false);
        platform::AscentPanel panel;
        model.input_sources = input.sources() + panel.sources();
        model.control_path = o.control;
        model.hud_style = o.vector ? "vector" : "canopy";
        // Passive HUD telemetry can rebuild large canopy images. Keep that
        // work off the menu thread; completed shared-memory images are leased
        // until the UI has finished drawing them.
        const auto hud_executable=std::filesystem::read_symlink("/proc/self/exe").parent_path() /
            (o.vector ? "kestrel-hud-vector" : "kestrel-hud");
        kestrel_ui::HudClient hud(screen_w,screen_h,o.assets,hud_executable.string());
        kestrel_ui::View view(controller, screen_w, screen_h, o.assets, &hud);
        model.backend.bf_available=view.betaflight_available();
        uint32_t bf_version=~uint32_t(0);
        uint32_t fc_last_frames=0;
        flight_controller.set_cell_size(1,1);
        if (o.menu)
            controller.open_menu();
        const auto start = kestrel_ui::monotonic_us();
        mode.started(start);
        if(mode.pending) {
            controller.open_menu(); model.side=0; model.tab=5; model.selected=3;
            model.stage=kestrel_ui::MenuStage::Browse;
        }
        uint64_t drawn = 0, frame_deadline=start;
        int previous_bind = gnd::BIND_IDLE;
        std::string recording_error;
        uint64_t profile_start = start, profile_loops = 0, profile_drawn = 0, profile_presented = 0, profile_ui_flips = 0;
        uint64_t render_us = 0, overlay_us = 0, longest_loop_us = 0;
        uint64_t longest_render_us = 0, longest_overlay_us = 0;
        while (!interrupted && !model.quit && !source_exited && !receiver.screen->failed() &&
               !receiver.decoder->failed()) {
            const auto now = kestrel_ui::monotonic_us();
            if(mode.tick(now)) break;
            air::FcChunk chunk;
            while(radio_model.fc().take(chunk)) {
                if(chunk.screen_reset) {
                    flight_controller.reset_screen();
                    model.backend.serial={}; model.backend.flight={};
                    fc_last_frames=flight_controller.msp_counts().frames;
                }
                if(chunk.n) flight_controller.parse_bytes(chunk.b,chunk.n);
            }
            const auto fc_counts=flight_controller.msp_counts();
            model.backend.serial.frames=fc_counts.frames;
            model.backend.serial.bytes=fc_counts.bytes;
            if(fc_counts.frames!=fc_last_frames) model.backend.serial.stamp_us=now;
            fc_last_frames=fc_counts.frames;
            model.backend.flight=flight_controller.get_telem();
            model.backend.flight_version=flight_controller.content_version();
            if(bf_version!=model.backend.flight_version) {
                bf_version=model.backend.flight_version; model.backend.bf_grid.fill(0);
                flight_controller.draw_region(0,0,20,53,0,0,[&](float x,float y,const std::vector<uint16_t>& span) {
                    const int row=int(-y),col=int(x);
                    if(row<0 || row>=20)return;
                    for(size_t i=0;i<span.size() && col+int(i)<53;++i) if(col+int(i)>=0)
                        model.backend.bf_grid[row*53+col+i]=span[i];
                });
            }
            if (o.seconds && now - start >= uint64_t(o.seconds) * 1000000)
                break;
            if (menu_requested) {
                menu_requested = 0;
                controller.key(kestrel_ui::Key::Menu);
            }
            model.backend.media.video_stamp_us = receiver.decoder->last_frame_us();
            const auto video = receiver.screen->video_stats();
            model.backend.video = {video.stamp_us, video.width, video.height, video.fps, video.latency_ms};
            const auto state = radio_model.radio().state();
            auto &telemetry = model.backend.vrx;
            telemetry.stamp_us = state.link.last_update_ms * 1000;
            telemetry.connected = state.link.active && state.link.state == gnd::kLinkUp;
            // The video arrives on RX; link.mcs is this receiver's uplink TX MCS.
            telemetry.mcs = telemetry.connected ? state.link.rx_mcs_val : -1;
            telemetry.frequency_mhz = state.link.tx_freq;
            flight_controller.set_motion_wanted(model.dynamic>0 && telemetry.link_valid(now));
            if(model.dynamic && telemetry.link_valid(now) && model.backend.media.video_fresh(now) && !model.menu_open) {
                constexpr float scale[]={0,.5f,1,3};bool moving=false;
                flight_controller.get_hud_offset(model.backend.hud_dx,model.backend.hud_dy,model.backend.hud_bank,scale[model.dynamic],moving);
            } else {
                flight_controller.reset_hud_motion();model.backend.hud_dx=model.backend.hud_dy=model.backend.hud_bank=0;
            }
            if(telemetry.link_valid(now)) {
                char details[128];std::snprintf(details,sizeof(details),"%.0f us / %s / %s MHz / MCS %d",state.link.frame_us,
                    state.link.frame_mode_asked==2?"SHORT":"STANDARD",gnd::bw_step_label(state.link.bw_step),state.link.rx_mcs_val);
                model.backend.radio_debug=details;
            } else model.backend.radio_debug="";
            // Native ranging is metres; the legacy raw field is not reused.
            telemetry.distance_raw = kestrel_ui::Telemetry::unavailable;
            model.backend.service.connected = o.radio;
            model.backend.service.provider_connected = telemetry.fresh(now);
            controls.update(model.backend, now);
            auto& screen_control=model.backend.controls["screen_mode"];
            screen_control={}; screen_control.known=true; screen_control.enabled=true;
            screen_control.value=mode.current;
            if(mode.pending) {
                screen_control.action=true;
                screen_control.text="KEEP " + model.screen_mode;
                screen_control.reason="Press OK to keep. Reverting in " + std::to_string((mode.deadline-now+999999)/1000000) + " seconds.";
            } else {
                screen_control.text=model.screen_mode;
                screen_control.choices=screen_choices;
                if(screen_control.choices.size()<2) screen_control.reason="Additional modes require compatible firmware, monitor and compiled assets.";
            }

            region_menu.update(model.backend);
            channel_menu.update(model.backend,now,model.channel_open);
            if(!playback_checked || now-playback_checked>=250000) {
                const bool was_busy=playback.busy;
                playback=player.state();playback_checked=now;
                model.backend.recordings.clear();
                for(const auto& clip:playback.recordings)model.backend.recordings.push_back({clip.name,clip.size});
                if(was_busy && !playback.busy && model.scene==kestrel_ui::SceneMode::GalleryPlayback) {
                    controller.set_scene(kestrel_ui::SceneMode::GalleryBrowse);
                    model.feedback=playback.error.empty()?"Playback finished":playback.error;
                }
            }
            model.backend.media.playback_known=true;model.backend.media.playing=playback.playing;
            model.backend.media.playback_id=playback.file;
            model.backend.playback_busy=playback.busy;model.backend.playback_paused=playback.paused;
            model.backend.playback_position_us=playback.position_us;model.backend.playback_duration_us=playback.duration_us;
            model.backend.playback_error=playback.error;
            auto& gallery=model.backend.controls["gallery"];gallery={};gallery.known=true;gallery.enabled=true;gallery.action=true;
            gallery.text=std::to_string(playback.recordings.size())+" RECORDINGS";
            const auto recording=recorder.state();
            model.backend.media.recording_known=true; model.backend.media.recording=recording.recording;
            model.backend.media.recording_id=recording.file;
            auto& rec=model.backend.controls["record"]; rec={};rec.known=true;rec.action=true;
            rec.enabled=recorder.is_recording() || (!playback.busy && recording.storage_ready && model.backend.media.video_fresh(now));
            rec.text=recorder.is_recording()?"STOP":recording.finishing?"SAVING":"START";
            rec.reason=recording.error.empty()?"Records the received video without re-encoding.":recording.error;
            auto& folder=model.backend.controls["folder"];folder={};folder.known=true;folder.text=o.recordings;
            auto& space=model.backend.controls["space"];space={};space.known=true;
            space.text=std::to_string(recording.free_bytes/(1024*1024))+" MiB";
            space.reason=recording.storage_ready?"":"Storage unavailable or less than 64 MiB free.";
            input.poll(controller, now);
            auto notice = [&](const char* text) {
                if (!model.menu_open) controller.open_menu();
                model.feedback = text;
                ++model.menu_version;
                printf("panel: %s\n", text);
            };
            if(recording.error!=recording_error) {
                recording_error=recording.error;
                if(!recording_error.empty()) {notice(recording_error.c_str());panel.beep(platform::PanelTone::Failure);}
            }
            for (const auto& event : panel.take()) {
                if (event.repeat && !model.key_repeat) continue;
                printf("panel: key=%s%s\n", bsp::ascent_panel::name(event.key), event.repeat ? " repeat" : "");
                if (event.key == platform::PanelKey::Rec) {
                    try {model.feedback=controller.apply_receiver("record",0);panel.beep();}
                    catch(const std::exception& e) {notice(e.what());panel.beep(platform::PanelTone::Failure);}
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
            const auto ui_now = kestrel_ui::monotonic_us();
            controller.tick(ui_now);
            const auto render_start = kestrel_ui::monotonic_us();
            view.update(ui_now);
            model.hud_pid=hud.pid();model.hud_restarts=hud.restarts();
            lv_timer_handler();
            lv_refr_now(display.lv_display());
            const auto overlay_start = kestrel_ui::monotonic_us();
            if (display.presentations != drawn) {
                drawn = display.presentations;
                receiver.screen->overlay_damage(display.pixels().data(), screen_w, screen_h, display.take_damage());
            }
            const auto rendered = kestrel_ui::monotonic_us();
            // Keep an absolute phase: sleep/wakeup overhead must not lower
            // a 60 Hz target on every frame. Skip stale deadlines on overruns.
            const uint64_t frame_us = 1000000 / std::max(1, model.fps);
            frame_deadline=kestrel_ui::next_frame_deadline(frame_deadline,rendered,frame_us);
            usleep(frame_deadline-rendered);
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
                    const auto ui_flips=receiver.screen->overlay_presented();
                    printf("profile: target=%d loops=%.2f/s UI=%.2f fps UI_scanout=%.2f/s HDMI_updates=%.2f/s "
                           "render=%.1fms overlay=%.1fms max_loop=%.1fms max_render=%.1fms max_overlay=%.1fms\n",
                           model.fps, profile_loops / seconds, (drawn - profile_drawn) / seconds,
                           (ui_flips-profile_ui_flips)/seconds, (presented - profile_presented) / seconds,
                           render_us / (1000.0 * profile_loops), overlay_us / (1000.0 * profile_loops),
                           longest_loop_us / 1000.0, longest_render_us / 1000.0, longest_overlay_us / 1000.0);
                    profile_start = ended; profile_drawn = drawn; profile_presented = presented;profile_ui_flips=ui_flips;
                    profile_loops = render_us = overlay_us = longest_loop_us = 0;
                    longest_render_us = longest_overlay_us = 0;
                }
            }
        }
        if (!o.screenshot.empty())
            display.dump(o.screenshot);
        const bool source_failed = source_exited.load();
        player.stop();
        workers.finish();
        printf("ascent: HDMI presentations=%llu UI presentations=%llu\n",
               (unsigned long long)receiver.screen->presented(), (unsigned long long)display.presentations);
        return source_failed || worker_failed || receiver.screen->failed() || receiver.decoder->failed() ? 1
                                                                                                         : 0;
    } catch(const std::exception& e) {
        fprintf(stderr,"screen session: %s\n",e.what()); return 1;
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        auto o = options(argc, argv);
        if (!o.command.empty()) {
            if (o.control.empty()) throw std::invalid_argument("--send-key requires --control");
            puts(kestrel_ui::send_control(o.control, o.command).c_str()); return 0;
        }
        if (!o.trace_dir.empty()) ltrace::start(o.trace_dir);
        signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler); signal(SIGUSR1, signal_handler);
        Settings settings;
        if (o.settings_file.empty() && !o.preferences.empty()) o.settings_file=o.preferences+".receiver";
        if (!o.settings_file.empty()) settings.load(o.settings_file);
        int height=o.explicit_mode?o.platform.display_height:settings.get(display::setting::ascent_screen_height);
        if(height!=720 && height!=1080)height=1080;
        display::ModeTrial mode(height);
        for(;;) {
            mode.restart=false;
            const int result=run_session(o,settings,mode);
            if(interrupted)return result;
            if(result && mode.pending) mode.revert();
            else if(result && mode.current!=1080) {
                mode.current=mode.confirmed=1080; mode.restart=true;
                fprintf(stderr,"screen: falling back to 1920x1080 @ 60 Hz\n");
            }
            if(!mode.restart)return result;
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "ascent: %s\n", e.what()); return 1;
    }
}

#include <atomic>
// Native Ascent composition root: existing radio, software HUD, platform media.
#include "base/ltrace.hpp"
#include "display/mode_trial.hpp"
#include "display/settings.hpp"
#include "platform/software.hpp"
#include "platform/ascent_panel.hpp"
#include "kestrel_gnd_config.h"
#include "radio/ar8030_link.hpp"
#include "radio/native_pairing.hpp"
#include "system/air/power.hpp"
#include "hud/lvgl/display.hpp"
#include "hud/lvgl/input.hpp"
#include "hud/lvgl/preferences.hpp"
#include "hud/lvgl/receiver_controls.hpp"
#include "ascent_menu.hpp"
#include "dvr/dvr_sink.hpp"
#include "dvr/stream_recorder.hpp"
#include "dvr/thumbnails.hpp"
#include "app/recorded_hud.hpp"
#include "dvr/settings.hpp"
#include "msp/msp_osd.hpp"
#include "motion/imu_reader.hpp"
#include "hal/source_decoder.hpp"
#include "dvr/stream_player.hpp"
#include "dvr/export_job.hpp"
#include "hal/v4l2/find_encoder.hpp"
#include "hud/lvgl/view.hpp"
#include "render/software/frame_cadence.hpp"
#include <condition_variable>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
volatile sig_atomic_t interrupted = 0, menu_requested = 0;
// Copies each UI frame's damage to the overlay plane on the other core while
// LVGL draws the next frame into its other buffer. The only overlay producer.
class OverlayCopier {
public:
    OverlayCopier(display::PlaneScreen& screen, unsigned width, unsigned height)
        : screen_(screen), width_(width), height_(height), thread_([this] { run(); }) {}
    ~OverlayCopier() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_all();
        thread_.join();
    }
    // Waits for the previous copy: it reads the buffer LVGL draws into next.
    void submit(const uint32_t* pixels, std::vector<base::Damage> damage) {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return !pending_; });
        if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
        pixels_ = pixels; damage_ = std::move(damage); pending_ = true;
        wake_.notify_all();
    }
    uint64_t copy_us() const { return copy_us_.load(); }
private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stop_ || pending_; });
            if (!pending_) return;
            const uint32_t* pixels = pixels_;
            const auto damage = std::move(damage_);
            lock.unlock();
            const auto started = kestrel_ui::monotonic_us();
            std::exception_ptr error;
            try { screen_.overlay_damage(pixels, width_, height_, damage); }
            catch (...) { error = std::current_exception(); }
            copy_us_ += kestrel_ui::monotonic_us() - started;
            lock.lock();
            if (error) error_ = error;
            pending_ = false;
            wake_.notify_all();
        }
    }
    display::PlaneScreen& screen_;
    const unsigned width_, height_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool pending_ = false, stop_ = false;
    const uint32_t* pixels_ = nullptr;
    std::vector<base::Damage> damage_;
    std::exception_ptr error_;
    std::atomic<uint64_t> copy_us_{0};
    std::thread thread_;
};
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
    o.link.pairing_file = "/var/lib/kestrel/peer";
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
            const auto mode=display::parse_ascent_mode(value());
            o.platform.display_width=mode.width();
            o.platform.display_height=mode.height;
            o.platform.display_hz=mode.hz;
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
        else if (arg == "--pairing")
            o.link.pairing_file = value();
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
            puts("Ascent VRX receiver (native fpvOS, 720p/1080p HDMI)\n"
                 "  --peer AA:BB:CC:DD   Air unit radio MAC, for this session\n"
                 "  --screen-mode MODE  1280x720@50/60/100/120 or 1920x1080@50/60\n"
                 "  --assets DIR        Generated LVGL assets\n"
                 "  --force-display     Bypass HDMI detection/EDID; retain driver mode checks\n"
                 "  --input /dev/input/eventN  Optional evdev buttons\n"
                 "  --control PATH      Optional local UI control socket\n"
                 "  --send-key KEY      Send menu/up/down/left/right/enter/back/quit to that socket\n"
                 "  --preferences FILE  Save visual preferences (optional)\n"
                 "  --show-menu --vector --no-radio --seconds N\n"
                 "  --pairing FILE      Persistent native pairing file\n"
                 "  --settings FILE     Save receiver and per-air-unit settings\n"
                 "  --recordings DIR    Existing folder for received-stream MP4 recordings\n"
                 "  --profile           Log UI and display timing every five seconds\n"
                 "  --trace-dir DIR     Watch DIR/ltrace.req; write frame timing to DIR/ltrace.bin\n"
                 "  --drm DEVICE --decoder DEVICE --replay FILE");
            exit(0);
        } else
            throw std::invalid_argument("unknown option: " + arg);
    }
    return o;
}

display::SessionResult run_session(Options o, Settings& settings, display::ModeTrial& mode) {
    try {
        o.platform.display_height = mode.current.height;
        o.platform.display_hz = mode.current.hz;
        o.platform.display_width = mode.current.width();
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
        std::atomic<bool> stop = false;
        const auto assets_root = std::filesystem::path(o.assets);
        const int screen_w=o.platform.display_width,screen_h=o.platform.display_height;
        const auto profile=std::filesystem::path(o.assets)/(std::to_string(screen_w)+"x"+std::to_string(screen_h));
        if(std::filesystem::is_directory(profile)) o.assets=profile.string();
        else if(screen_h!=1080)throw display::ScreenError("Install generated 720p UI assets before selecting this mode");
        const VideoCodec codec = VideoCodec::H265;
        dvr::Thumbnails thumbnails(o.recordings.empty()?settings.get(dvr::setting::dvr_dir):o.recordings);
        struct PreviewSink : hal::FrameSink {
            dvr::Thumbnails& thumbnails; std::shared_ptr<hal::FrameSink> screen;
            std::atomic<uint64_t> playback_floor{0};
            std::atomic<uint64_t> decoded_pts{0};
            PreviewSink(dvr::Thumbnails& t,std::shared_ptr<hal::FrameSink> s):thumbnails(t),screen(std::move(s)) {}
            void queue_frame(std::shared_ptr<DecodedUnit> frame) override {
                decoded_pts=uint64_t(frame->pts)+1;
                if(uint64_t(frame->pts)<playback_floor.load())return;
                thumbnails.capture(frame);screen->queue_frame(std::move(frame));
            }
            void request_flush() override {screen->request_flush();decoded_pts=0;}
        };
        auto receiver = platform::make_software_receiver(o.platform, codec, &stop);
        auto preview_sink=std::make_shared<PreviewSink>(thumbnails,receiver.screen);
        auto source_decoder=std::make_shared<hal::SourceDecoder>([&] {
            return platform::make_software_decoder(o.platform,codec,preview_sink,&stop);
        },&stop);
        receiver.decoder=source_decoder;
        if(o.recordings.empty()) o.recordings=settings.get(dvr::setting::dvr_dir);
        dvr::StreamRecorder recorder(o.recordings,codec,1920,1080,60);
        dvr::StreamPlayer player(o.recordings,{
            [&](bool playing){thumbnails.cancel();preview_sink->playback_floor=0;receiver.screen->hold_video(playing);source_decoder->playback(playing);},
            [&]{return source_decoder->playback_ready();},
            [&]{return source_decoder->playback_failed() || source_decoder->failed();},
            [&](const std::vector<uint8_t>& data,uint64_t pts,uint8_t nal,bool key) {
                source_decoder->feed_playback(const_cast<uint8_t*>(data.data()),int(data.size()),pts,kestrel_ui::monotonic_us(),nal,key);
            },[&](bool paused){source_decoder->pause_input(paused);},
            [&](const std::string& name){thumbnails.arm(name,kestrel_ui::monotonic_us());},
            [&](uint64_t pts){preview_sink->playback_floor=pts;source_decoder->restart_playback();},
            [&]{return preview_sink->decoded_pts.load();}});
        DvrSink recording_sink(codec,recorder);
        Ar8030Link link(radio_model, o.link, settings, codec, receiver.decoder, &stop);
        link.add_video_sink(&recording_sink);
        std::atomic<bool> source_exited{false}, worker_failed{false};
        struct Join {
            std::atomic<bool> &stop;
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
        const auto export_executable=std::filesystem::read_symlink("/proc/self/exe").parent_path()/"kestrel-export";
        auto exporter=std::make_unique<dvr::ExportJob>(o.recordings,export_executable.string(),dvr::ExportJob::Media{
            [&](bool on){source_decoder->suspend(on);},[&]{return source_decoder->suspended();}});
        const auto export_encoder=hal::find_hevc_encoder();
        const bool export_available=exporter->available() && access("/dev/ar9311-ge2d",R_OK|W_OK)==0 && !export_encoder.empty();
        uint64_t export_revision=0;
        lv_init();
        struct LvLifetime { ~LvLifetime() { lv_deinit(); } } lv_lifetime;
        lv_tick_set_cb([] { return uint32_t(kestrel_ui::monotonic_us() / 1000); });
        kestrel_ui::Display display(screen_w, screen_h, "", 0, 2);
        OverlayCopier copier(*receiver.screen, unsigned(screen_w), unsigned(screen_h));
        kestrel_ui::Model model;
        model.build = kestrel_gnd_build_time;
        model.version = std::string(kestrel_gnd_version) + " (" + kestrel_gnd_git_hash + ")";
        model.decoder = "V4L2 / Wave5";
        model.screen_mode = mode.current.label();
        model.backend.media.video_monitored = o.radio;
        model.backend.service.adapter = "fpvOS native Ascent";
        model.backend.service.hardware_delegated = false;
        model.backend.vrx.freshness_us = 6000000; // native full radio poll is every three seconds
        try { model.set_preferences(kestrel_ui::load_preferences(o.preferences)); }
        catch(const std::exception& error) {
            fprintf(stderr,"preferences: %s; starting with defaults\n",error.what());
        }
        kestrel_ui::Controller controller(model);
        ascent::RegionMenu region_menu;
        ascent::ChannelMenu channel_menu(radio_model.radio());
        dvr::StreamPlayer::State playback;
        uint64_t playback_checked=0;
        std::vector<kestrel_ui::ReceiverControls::VideoMode> video_modes;
        for(int i=0;i<sky::kFpvModeCount;i++) video_modes.push_back({sky::kFpvModes[i].name,sky::kFpvModes[i].need});
        kestrel_ui::ReceiverControls controls(radio_model.camera(),radio_model.encoder(),radio_model.air_radio(),radio_model.air_unit(),radio_model.radio(),
                                              std::move(video_modes));
        std::vector<kestrel_ui::ReceiverChoice> screen_choices;
        for(const auto candidate:display::ascent_modes) if(receiver.screen->supports_mode(candidate) &&
            std::filesystem::is_directory(assets_root/(candidate.height==720?"1280x720":"1920x1080")))
            screen_choices.push_back({candidate.id(),candidate.label()});
        std::string temperature_path;
        uint64_t temperature_next=0,temperature_search=0;
        controller.apply_receiver=[&](const std::string& id,int value) {
            if(id=="export_cancel") {exporter->cancel();return std::string("Canceling export...");}
            if(exporter->state().busy)throw std::runtime_error("Wait for export or cancel it first");
            if(id.compare(0,7,"export:")==0) {
                const auto rec=recorder.state();const auto play=player.state();
                if(recorder.is_recording() || rec.finishing || play.busy || play.deleting || mode.pending)
                    throw std::runtime_error("Stop recording/playback and finish screen changes before exporting");
                if(access("/dev/ar9311-ge2d",R_OK|W_OK) || export_encoder.empty())
                    throw std::runtime_error("Hardware export needs updated fpvOS firmware");
                exporter->start(id.substr(7),{assets_root.string(),o.preferences.empty()?"-":o.preferences,
                    o.platform.decoder_device,export_encoder,o.vector?"vector":"canopy"});
                model.backend.export_busy=true;model.backend.export_percent=0;
                model.backend.export_file=id.substr(7);model.backend.export_message="Preparing export";
                return std::string("Export started");
            }
            if(id=="screen_mode") {
                if(mode.pending) {
                    mode.keep();
                    settings.set(display::setting::ascent_screen_height,mode.current.height);
                    settings.set(display::setting::ascent_screen_hz,mode.current.hz);
                    settings.flush();
                    return std::string("Screen mode saved");
                }
                if(recorder.is_recording() || recorder.state().finishing || player.state().busy || player.state().deleting)
                    throw std::runtime_error("Wait for recording, playback or deletion to finish before changing screen mode");
                if(std::none_of(screen_choices.begin(),screen_choices.end(),[&](const auto& choice){return choice.value==value;}))
                    throw std::runtime_error("Screen mode or compiled assets unavailable");
                mode.select(display::AscentMode::from_id(value)); model.quit=mode.restart;
                return std::string("Testing screen mode");
            }
            if(id=="record") {
                if(player.state().deleting)throw std::runtime_error("Wait for recording deletion to finish");
                // A trial that runs out restarts the session, which would end
                // a recording started during it.
                if(!recorder.is_recording() && mode.pending)
                    throw std::runtime_error("Keep or revert the new screen mode first");
                if(!recorder.is_recording() && (player.state().busy || !model.backend.media.video_fresh(kestrel_ui::monotonic_us()) || !recorder.state().storage_ready))
                    throw std::runtime_error("Recording needs live video and writable storage");
                recorder.toggle(); return std::string("Recording change requested");
            }
            if(id.compare(0,5,"play:")==0) {
                const auto rec=recorder.state();
                if(recorder.is_recording() || rec.finishing)throw std::runtime_error("Stop recording and wait for it to finish saving first");
                if(mode.pending)throw std::runtime_error("Keep or revert the new screen mode first");
                player.play(id.substr(5));playback=player.state();playback_checked=0;return std::string("Opening recording");
            }
            if(id.compare(0,7,"delete:")==0) {
                const auto rec=recorder.state();
                if(recorder.is_recording() || rec.finishing)throw std::runtime_error("Stop recording and wait for saving before deleting");
                player.erase(id.substr(7));playback_checked=0;
                return std::string("Deleting recording...");
            }
            if(id=="playback_stop") {player.stop();return std::string("Playback stopped");}
            if(id=="playback_pause") {player.pause();return std::string("Playback pause changed");}
            if(id=="playback_seek") {
                player.seek_relative(value,model.backend.playback_position_us);
                playback_checked=0;return std::string("Playback seek requested");
            }
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
            controller.open_menu();
            if(model.focus_row("screen_mode",start)) model.stage=kestrel_ui::MenuStage::Browse;
        }
        uint64_t drawn = 0, frame_deadline=start;
        bool was_browsing=false; int recording_listed=0;
        int previous_bind = gnd::BIND_IDLE;
        std::string recording_error,telemetry_error,thumbnail_session,unsupported_video,held_notice;
        std::vector<uint8_t> playback_hud_bytes;
        std::shared_ptr<const kestrel_ui::HudData> playback_hud;
        uint64_t profile_start = start, profile_loops = 0, profile_drawn = 0, profile_presented = 0, profile_ui_flips = 0;
        uint64_t render_us = 0, overlay_us = 0, longest_loop_us = 0;
        uint64_t longest_render_us = 0, longest_overlay_us = 0;
        uint64_t rate_start = start, rate_loops = 0, rate_slowest_us = 0, profile_copy_us = 0;
        while (!interrupted && !model.quit && !source_exited && !receiver.screen->failed() &&
               !receiver.decoder->failed()) {
            const auto now = kestrel_ui::monotonic_us();
            // A screen mode on trial is kept when the craft arms, as on the VRX
            // Pro; one tried while armed is not reverted in the air (that
            // restarts the session, radio and HDMI included) but once it has
            // landed.
            const bool arming=controller.take_arming();
            if(arming && mode.pending) {
                mode.keep();
                settings.set(display::setting::ascent_screen_height,mode.current.height);
                settings.set(display::setting::ascent_screen_hz,mode.current.hz);
                settings.flush();
                printf("menu: screen mode %s kept (armed)\n",mode.current.label().c_str());
            }
            // An export holds the live decoder: arming brings the video back.
            if(arming && exporter->state().busy) {
                exporter->cancel();
                printf("export: canceled (armed)\n");
            }
            if(mode.pending && mode.deadline && now>=mode.deadline && controller.armed(now)) mode.deadline=now+1000000;
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
            // Decoded pictures the display drops (another size) are no video
            // on the screen: the idle status shows, not RECEIVING VIDEO.
            if(video.rejected_us && now>=video.rejected_us && now-video.rejected_us<2000000)
                model.backend.media.video_stamp_us=0;
            model.backend.video = {video.stamp_us, video.width, video.height, video.fps, video.latency_ms};
            const auto state = radio_model.radio().state();
            auto &telemetry = model.backend.vrx;
            // The receiver's SoC temperature (fpvOS ar9311_temp, hwmon), once
            // a second; looked for again every 10 s while it is not there.
            if(now>=temperature_next) {
                temperature_next=now+1000000;
                if(temperature_path.empty() && now>=temperature_search) {
                    temperature_search=now+10000000;
                    std::error_code ec;
                    for(const auto& entry:std::filesystem::directory_iterator("/sys/class/hwmon",ec)) {
                        std::ifstream name(entry.path()/"name");std::string text;
                        if(std::getline(name,text) && text=="ar9311_temp") {temperature_path=(entry.path()/"temp1_input").string();break;}
                    }
                }
                long millidegrees=0;
                std::ifstream input(temperature_path);
                if(!temperature_path.empty() && (input>>millidegrees) && millidegrees>-40000 && millidegrees<150000) {
                    telemetry.cpu_temperature_x100=int(millidegrees/10);telemetry.local_stamp_us=now;
                } else {
                    telemetry.cpu_temperature_x100=kestrel_ui::Telemetry::unavailable;
                    if(!temperature_path.empty()) temperature_path.clear();
                }
            }
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
                // rx_mcs_val is the radio's index: the MCS it names is two under it.
                char details[128];std::snprintf(details,sizeof(details),"%.0f us / %s / %s MHz / MCS %d",state.link.frame_us,
                    state.link.frame_mode_asked==2?"SHORT":"STANDARD",gnd::bw_step_label(state.link.bw_step),ar_mcs_label(state.link.rx_mcs_val));
                model.backend.radio_debug=details;
            } else model.backend.radio_debug="";
            // Native ranging is metres; the legacy raw field is not reused.
            telemetry.distance_raw = kestrel_ui::Telemetry::unavailable;
            model.backend.service.connected = o.radio;
            model.backend.service.provider_connected = telemetry.fresh(now);
            controls.update(model.backend, now);
            auto& screen_control=model.backend.controls["screen_mode"];
            screen_control={}; screen_control.known=true; screen_control.enabled=true;
            screen_control.value=mode.current.id();
            if(mode.pending) {
                screen_control.action=true;
                screen_control.text="KEEP " + model.screen_mode;
                screen_control.reason="Press OK to keep. Reverting in " + std::to_string((mode.deadline-now+999999)/1000000) + " seconds.";
            } else {
                screen_control.text=model.screen_mode;
                screen_control.choices=screen_choices;
                if(screen_control.choices.size()<2) screen_control.reason="Additional modes require compatible firmware, monitor and compiled assets.";
            }

            const auto exporting=exporter->state();
            model.backend.export_busy=exporting.busy;model.backend.export_percent=exporting.percent;
            model.backend.export_file=exporting.file;model.backend.export_message=exporting.message;
            auto& export_control=model.backend.controls["recording_export"];
            export_control={};export_control.known=true;export_control.action=true;
            export_control.enabled=!exporting.busy && export_available;
            if(exporting.revision!=export_revision){export_revision=exporting.revision;model.feedback=exporting.message;}
            region_menu.update(model.backend);
            channel_menu.update(model.backend,now,model.channel_open);
            if(!playback_checked || now-playback_checked>=250000) {
                const bool was_busy=playback.busy;
                const auto deletion_revision=playback.deletion_revision;
                playback=player.state();playback_checked=now;
                model.backend.recordings.clear();
                for(const auto& clip:playback.recordings)model.backend.recordings.push_back({clip.name,clip.size});
                model.gallery_selected=std::clamp(model.gallery_selected,0,std::max(0,int(playback.recordings.size())-1));
                if(playback.deletion_revision!=deletion_revision && model.scene==kestrel_ui::SceneMode::GalleryBrowse)
                    model.feedback=playback.deletion_message;
                if(was_busy && !playback.busy && model.scene==kestrel_ui::SceneMode::GalleryPlayback) {
                    controller.set_scene(kestrel_ui::SceneMode::GalleryBrowse);
                    model.feedback=playback.error.empty()?"Playback finished":playback.error;
                }
            }
            model.backend.media.playback_known=true;model.backend.media.playing=playback.playing;
            model.backend.media.playback_id=playback.file;
            model.backend.playback_busy=playback.busy;model.backend.playback_paused=playback.paused;
            model.backend.playback_seeking=playback.seeking;
            model.backend.playback_position_us=playback.seeking?playback.seek_target_us:playback.position_us;
            model.backend.playback_duration_us=playback.duration_us;
            model.backend.playback_error=playback.error;
            auto& gallery=model.backend.controls["gallery"];gallery={};gallery.known=true;gallery.enabled=true;gallery.action=true;
            gallery.text=std::to_string(playback.recordings.size())+" RECORDINGS";
            const auto recording=recorder.state();
            // The recordings' folder is listed when someone looks at it (the
            // menu or the gallery opens) or a recording starts or is saved,
            // not twice a second from boot.
            const bool browsing=model.menu_open || model.scene==kestrel_ui::SceneMode::GalleryBrowse;
            const int listed_state=(recording.recording?1:0)|(recording.finishing?2:0);
            if((browsing && !was_browsing) || listed_state!=recording_listed) player.refresh();
            was_browsing=browsing; recording_listed=listed_state;
            const auto thumbnail_id=recording.recording && recording.frames?recording.file:std::string();
            if(thumbnail_id!=thumbnail_session) {
                thumbnails.cancel();thumbnail_session=thumbnail_id;
                if(!thumbnail_id.empty())thumbnails.arm(std::filesystem::path(recording.file).filename().string(),now);
            }
            if(model.scene==kestrel_ui::SceneMode::GalleryBrowse) {
                const int first=std::max(0,model.gallery_selected)/6*6;
                for(int i=0;i<int(model.backend.recordings.size());++i) {
                    auto& clip=model.backend.recordings[i];
                    clip.thumbnail=i>=first && i<first+6?thumbnails.get(clip.name,320*screen_h/1080,180*screen_h/1080):nullptr;
                }
            }
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
            // A notice opens the menu with it - but not over the picture in
            // flight: armed, the last one waits, and shows once disarmed.
            auto notice = [&](const char* text) {
                printf("panel: %s\n", text);
                if (controller.armed(now)) { held_notice = text; return; }
                // Over the gallery it is said there: opening the menu would
                // leave a recording playing behind it with no way to stop it.
                const bool gallery = model.scene == kestrel_ui::SceneMode::GalleryBrowse ||
                                     model.scene == kestrel_ui::SceneMode::GalleryPlayback;
                if (!model.menu_open && !gallery) controller.open_menu();
                model.feedback = text;
                ++model.menu_version;
            };
            if (!held_notice.empty() && controller.arm_state(now) == 0) {
                const std::string text = std::move(held_notice);
                held_notice.clear();
                notice(text.c_str());
            }
            const std::string unsupported=video.rejected_us ?
                std::to_string(video.rejected_width)+"x"+std::to_string(video.rejected_height) : std::string();
            if(unsupported!=unsupported_video) {
                unsupported_video=unsupported;
                if(!unsupported.empty()) notice(("Video "+unsupported+" cannot be shown on this screen").c_str());
            }
            if(recording.telemetry_error!=telemetry_error) {
                telemetry_error=recording.telemetry_error;
                if(!telemetry_error.empty()){notice(telemetry_error.c_str());panel.beep(platform::PanelTone::Failure);}
            }
            if(recording.error!=recording_error) {
                recording_error=recording.error;
                if(!recording_error.empty()) {notice(recording_error.c_str());panel.beep(platform::PanelTone::Failure);}
            }
            for (const auto& event : panel.take()) {
                if (event.repeat && !model.key_repeat) continue;
                printf("panel: key=%s%s\n", bsp::ascent_panel::name(event.key), event.repeat ? " repeat" : "");
                if (event.key == platform::PanelKey::Rec) {
                    if(event.repeat)continue;
                    if(model.scene==kestrel_ui::SceneMode::GalleryBrowse) {
                        controller.key(kestrel_ui::Key::Record);panel.beep();continue;
                    }
                    // Not a recording key while one plays (nothing to record).
                    if(model.scene==kestrel_ui::SceneMode::GalleryPlayback) continue;
                    try {model.feedback=controller.apply_receiver("record",0);panel.beep();}
                    catch(const std::exception& e) {notice(e.what());panel.beep(platform::PanelTone::Failure);}
                } else if (event.key == platform::PanelKey::Upgrade) {
                    notice("Button-triggered upgrade is not available yet");
                    panel.beep(platform::PanelTone::Failure);
                } else if (event.key == platform::PanelKey::Link) {
                    // The over-video bind overlay (model.bind_state) shows the
                    // progress; the bind button must not open the menu.
                    if (o.radio && o.link.replay_path.empty()) {
                        radio_model.radio().bind();
                        printf("panel: bind button -> binding\n");
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
            // The bind overlay draws from these; it replaces the menu notices
            // so binding never pops the menu over the video.
            model.bind_state = state.bind_state;
            model.bind_secs_left = state.bind_secs_left;
            model.bind_peer = state.bind_peer;
            if (state.bind_state != previous_bind) {
                previous_bind = state.bind_state;
                ++model.menu_version;
                switch (state.bind_state) {
                case gnd::BIND_RUNNING:
                    printf("panel: binding (press LINK again to cancel)\n");
                    panel.beep(platform::PanelTone::Notice); break;
                case gnd::BIND_OK:
                    printf("panel: binding succeeded\n");
                    panel.beep(platform::PanelTone::Success); break;
                case gnd::BIND_FAILED:
                    printf("panel: binding failed\n");
                    panel.beep(platform::PanelTone::Failure); break;
                case gnd::BIND_IDLE: printf("panel: binding ended\n"); break;
                default: break;
                }
            }
            const auto ui_now = kestrel_ui::monotonic_us();
            controller.tick(ui_now);
            if(recorder.is_recording()) {
                try {recorder.set_telemetry(app::record_hud(controller,model,ui_now));}
                catch(const std::exception& e) {
                    recorder.set_telemetry(std::make_shared<media::TelemetrySample>());
                    fprintf(stderr,"telemetry snapshot: %s\n",e.what());
                }
            }
            model.backend.playback_hud.reset();model.backend.playback_osd_fresh=false;
            model.backend.playback_dx=model.backend.playback_dy=model.backend.playback_bank=0;
            model.backend.playback_warning=0;model.backend.playback_clock.clear();
            model.backend.playback_telemetry_error=playback.telemetry_error;
            if(playback.playing && !playback.seeking && video.stamp_us>=playback.started_us && video.pts_us>=playback.present_from_us) {
                model.backend.playback_position_us=video.pts_us;
                if(auto sample=player.telemetry_at(video.pts_us)) {
                    try {
                        if(sample->hud!=playback_hud_bytes) {
                            playback_hud=std::make_shared<kestrel_ui::HudData>(kestrel_ui::hud_ipc::decode_data(sample->hud));
                            playback_hud_bytes=sample->hud;
                        }
                        model.backend.playback_hud=playback_hud;
                        model.backend.playback_grid=sample->grid;
                        model.backend.playback_osd_fresh=app::reading(*sample,"osd.fresh")!=0;
                        model.backend.playback_dx=std::clamp(app::reading(*sample,"motion.dx"),-1.0,1.0);
                        model.backend.playback_dy=std::clamp(app::reading(*sample,"motion.dy"),-1.0,1.0);
                        model.backend.playback_bank=std::clamp(app::reading(*sample,"motion.bank_rad"),-1.0,1.0);
                        model.backend.playback_warning=int(std::clamp(app::reading(*sample,"link.warning"),0.0,2.0));
                        const auto clock=sample->text.find("clock");if(clock!=sample->text.end())model.backend.playback_clock=clock->second;
                    }catch(const std::exception& e){model.backend.playback_telemetry_error=e.what();}
                }
            }
            const auto render_start = kestrel_ui::monotonic_us();
            view.update(ui_now);
            model.hud_pid=hud.pid();model.hud_restarts=hud.restarts();
            const auto presented = display.presentations;
            lv_timer_handler();
            // LVGL pauses its refresh timer until something is invalidated.
            // Refreshing anyway is not free with two buffers: it would copy
            // the whole last frame into the other one, which the next frame
            // does for less (leaving out what it redraws). Nor when the timer
            // has just drawn this frame itself (a layout in that refresh
            // leaves it unpaused): a second one would copy into the buffer
            // the overlay copier may still be reading.
            if (display.presentations == presented &&
                !lv_timer_get_paused(lv_display_get_refr_timer(display.lv_display())))
                lv_refr_now(display.lv_display());
            const auto overlay_start = kestrel_ui::monotonic_us();
            if (display.presentations != drawn) {
                drawn = display.presentations;
                copier.submit(display.pixels().data(), display.take_damage());
            }
            const auto rendered = kestrel_ui::monotonic_us();
            // Keep an absolute phase: sleep/wakeup overhead must not lower
            // a 60 Hz target on every frame. Skip stale deadlines on overruns.
            const uint64_t frame_us = 1000000 / std::max(1, model.fps);
            frame_deadline=kestrel_ui::next_frame_deadline(frame_deadline,rendered,frame_us);
            usleep(frame_deadline-rendered);
            const auto ended = kestrel_ui::monotonic_us();
            // The FPS counter: loops (one a refresh while they keep up) and the
            // slowest one's work, its sleep left out, over the last second.
            ++rate_loops; rate_slowest_us = std::max(rate_slowest_us, rendered - now);
            if (ended - rate_start >= 1000000) {
                if (model.fps_counter) {
                    char rate[48];
                    std::snprintf(rate, sizeof(rate), "%.0f FPS  MAX %.1f MS", rate_loops * 1e6 / double(ended - rate_start),
                                  rate_slowest_us / 1000.0);
                    model.backend.frame_rate = rate;
                } else model.backend.frame_rate.clear();
                rate_start = ended; rate_loops = rate_slowest_us = 0;
            }
            if (o.profile) {
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
                    // overlay: the UI thread's handover (a wait for the previous
                    // copy); copy: the copier's own time, on the other core.
                    const auto copied_us = copier.copy_us();
                    printf("profile: target=%d loops=%.2f/s UI=%.2f fps UI_scanout=%.2f/s HDMI_updates=%.2f/s "
                           "render=%.1fms overlay=%.1fms copy=%.1fms max_loop=%.1fms max_render=%.1fms max_overlay=%.1fms\n",
                           model.fps, profile_loops / seconds, (drawn - profile_drawn) / seconds,
                           (ui_flips-profile_ui_flips)/seconds, (presented - profile_presented) / seconds,
                           render_us / (1000.0 * profile_loops), overlay_us / (1000.0 * profile_loops),
                           (copied_us - profile_copy_us) / (1000.0 * profile_loops),
                           longest_loop_us / 1000.0, longest_render_us / 1000.0, longest_overlay_us / 1000.0);
                    profile_copy_us = copied_us;
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
        exporter.reset();
        workers.finish();
        printf("ascent: HDMI presentations=%llu UI presentations=%llu\n",
               (unsigned long long)receiver.screen->presented(), (unsigned long long)display.presentations);
        if (receiver.screen->failed()) return display::SessionResult::DisplayFailed;
        return source_failed || worker_failed || receiver.decoder->failed() ? display::SessionResult::Failed
                                                                           : display::SessionResult::Stopped;
    } catch(const display::ScreenError& e) {
        fprintf(stderr,"screen session: %s\n",e.what()); return display::SessionResult::DisplayFailed;
    } catch(const std::exception& e) {
        fprintf(stderr,"receiver session: %s\n",e.what()); return display::SessionResult::Failed;
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
        setvbuf(stdout,nullptr,_IOLBF,0);
        if (!o.trace_dir.empty()) ltrace::start(o.trace_dir);
        signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler); signal(SIGUSR1, signal_handler);
        Settings settings;
        if (o.settings_file.empty() && !o.preferences.empty()) o.settings_file=o.preferences+".receiver";
        if (!o.settings_file.empty()) settings.load(o.settings_file);
        display::AscentMode selected = o.explicit_mode ?
            display::AscentMode{o.platform.display_height,o.platform.display_hz} :
            display::AscentMode{settings.get(display::setting::ascent_screen_height),settings.get(display::setting::ascent_screen_hz)};
        if(!selected.valid()) selected={};
        display::ModeTrial mode(selected);
        for(;;) {
            mode.restart=false;
            const auto result=run_session(o,settings,mode);
            const int status=result==display::SessionResult::Stopped?0:1;
            if(interrupted)return status;
            if(mode.recover(result))
                fprintf(stderr,"screen: recovering display at %s\n",mode.current.label().c_str());
            if(!mode.restart)return status;
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "ascent: %s\n", e.what()); return 1;
    }
}

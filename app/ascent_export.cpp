// Offline export has its own LVGL instance and bounded hardware session. HDMI
// stays owned by kestrel-gnd. Decoder references are never modified.
#include "dvr/ascent_export_writer.hpp"
#include "dvr/export_source.hpp"
#include "dvr/telemetry_track.hpp"
#include "hal/artosyn/dma_surface.hpp"
#include "hal/v4l2/vdec_v4l2.hpp"
#include "hud/lvgl/display.hpp"
#include "hud/lvgl/preferences.hpp"
#include "hud/lvgl/view.hpp"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <sys/prctl.h>
#include <thread>
using namespace kestrel_ui;
namespace {
volatile sig_atomic_t canceled = 0;
uint32_t video_ms = 0;
void signal_stop(int) { canceled = 1; }
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Sink final : hal::FrameSink {
    std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<DecodedUnit> frame;
    void queue_frame(std::shared_ptr<DecodedUnit> f) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (frame)
            throw std::runtime_error("Unexpected reordered decoder output");
        frame = std::move(f);
        wake.notify_all();
    }
    void request_flush() override {
        std::lock_guard<std::mutex> lock(mutex);
        frame.reset();
        wake.notify_all();
    }
    std::shared_ptr<DecodedUnit> take(VdecV4l2 &decoder) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::unique_lock<std::mutex> lock(mutex);
        while (!frame) {
            require(!canceled, "Export canceled");
            require(!decoder.failed(), "Export decoder failed");
            require(std::chrono::steady_clock::now() < deadline,
                    "Export decoder timeout (unsupported or damaged recording)");
            wake.wait_for(lock, std::chrono::milliseconds(20));
        }
        return std::exchange(frame, {});
    }
};
struct Decode {
    std::atomic<bool> stop{false};
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();
    VdecV4l2 decoder;
    std::thread thread;
    explicit Decode(const char *device)
        : decoder(VideoCodec::H265, device, sink, &stop, true), thread([&] { decoder.run_frame(); }) {}
    ~Decode() {
        stop = true;
        decoder.cleanup();
        if (thread.joinable())
            thread.join();
    }
};
// The HUD of the last sample shown: decoded again only when its bytes change
// (samples last many frames).
struct HudCache {
    std::shared_ptr<const media::TimedTelemetry> sample;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const HudData> data;
};
void hud_sample(Model &m, HudCache &cache, std::shared_ptr<const media::TimedTelemetry> s, uint64_t us) {
    auto &b = m.backend;
    b.playback_position_us = us;
    b.playback_hud.reset();
    b.playback_osd_fresh = false;
    b.playback_dx = b.playback_dy = b.playback_bank = 0;
    b.playback_warning = 0;
    b.playback_clock.clear();
    // A snapshot that failed while recording is stored empty (ascent_main):
    // the last good one stays on screen while it is fresh enough.
    if (s && s->sample.hud.empty())
        s = cache.sample;
    if (!s || us < s->us || us - s->us > 2500000)
        return;
    const auto &sample = s->sample;
    if (s != cache.sample) {
        cache.sample = s;
        if (sample.hud != cache.bytes) {
            cache.data = std::make_shared<HudData>(hud_ipc::decode_data(sample.hud));
            cache.bytes = sample.hud;
        }
    }
    b.playback_hud = cache.data;
    b.playback_grid = sample.grid;
    auto get = [&](const char *key) {
        const auto i = sample.values.find(key);
        return i == sample.values.end() ? 0. : i->second;
    };
    b.playback_osd_fresh = get("osd.fresh") != 0;
    b.playback_dx = std::clamp(get("motion.dx"), -1., 1.);
    b.playback_dy = std::clamp(get("motion.dy"), -1., 1.);
    b.playback_bank = std::clamp(get("motion.bank_rad"), -1., 1.);
    b.playback_warning = int(std::clamp(get("link.warning"), 0., 2.));
    const auto clock = sample.text.find("clock");
    if (clock != sample.text.end())
        b.playback_clock = clock->second;
}
void run(int argc, char **argv) {
    require(argc == 8, "Usage: kestrel-export INPUT OUTPUT.part ASSETS PREFERENCES DECODER ENCODER STYLE");
    dvr::ExportSource source(argv[1]);
    require((source.width == 1920 && source.height == 1080) || (source.width == 1280 && source.height == 720),
            "Export supports 720p and 1080p recordings");
    dvr::TelemetryReader telemetry(argv[1]);
    require(telemetry.available(), "Recorded HUD data is missing or invalid");
    const auto profile =
        std::filesystem::path(argv[3]) / (std::to_string(source.width) + "x" + std::to_string(source.height));
    require(std::filesystem::is_directory(profile), "Export UI assets unavailable for recording dimensions");
    // Initialize render objects before starting the decoder thread: LVGL belongs
    // exclusively to this thread. The UI process uses a separate LVGL instance.
    Display display(source.width, source.height, "");
    Model model;
    if (std::string(argv[4]) != "-")
        model.set_preferences(load_preferences(argv[4]));
    model.show_hud = true;
    model.scene = SceneMode::GalleryPlayback;
    model.playback_controls_visible = false;
    model.backend.media.playing = true;
    model.backend.playback_busy = true;
    model.hud_style = argv[7];
    Controller controller(model);
    InlineHud hud(source.width, source.height, profile.string(),
                  model.hud_style == "vector" ? HudStyle::Vector : HudStyle::Canopy);
    View view(controller, source.width, source.height, profile.string(), &hud);
    const unsigned coded = (source.height + 15) & ~15u;
    hal::Ge2dCompositor compositor;
    hal::DmaSurface output(source.width, coded * 3 / 2, 8), overlay(source.width, source.height, 32);
    auto destination = output.yuv(source.width, source.height, coded);
    hal::Ge2dOverlay panel;
    panel.image.width = source.width;
    panel.image.height = source.height;
    panel.image.fd[0] = overlay.fd;
    panel.image.stride[0] = overlay.stride;
    panel.image.format = AR_GE2D_ARGB8888;
    // Padding is initialized once; neither composition nor CPU copies video.
    output.sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    memset(output.data, 0, output.bytes);
    output.sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    dvr::AscentExportWriter encoder;
    require(source.duration_us > 0, "Invalid recording duration");
    unsigned fps =
        std::clamp<unsigned>(std::lround(double(source.count) * 1000000 / source.duration_us), 1, 120);
    encoder.init(argv[6], argv[2], source.width, source.height, fps);
    Decode decode(argv[5]);
    for (auto &p : source.parameters)
        decode.decoder.feed_packet_to_decoder(p.data(), p.size(), 0, 0, (p[4] >> 1) & 63, 0, 0);
    HudCache hud_cache;
    bool had_hud = false;
    int percent = -1;
    uint64_t presented = ~uint64_t(0);
    const auto started = std::chrono::steady_clock::now();
    auto reported = started;
    for (unsigned i = 0; i < source.count; ++i) {
        require(!canceled, "Export canceled");
        auto sample = source.sample(i);
        decode.decoder.feed_packet_to_decoder(sample.bytes.data(), sample.bytes.size(), sample.us, 0,
                                              sample.nal, 0, 0, sample.key);
        auto frame = decode.sink->take(decode.decoder);
        require(frame->pts == sample.us && frame->width == source.width && frame->height == source.height &&
                    frame->has_prime_fd,
                "Export decoder picture/timestamp mismatch");
        auto recorded = telemetry.at(sample.us);
        require(telemetry.error().empty(), "Recorded HUD data is damaged");
        hud_sample(model, hud_cache, recorded, sample.us);
        had_hud |= bool(model.backend.playback_hud);
        video_ms = 1000 + sample.us / 1000;
        view.update(1000000 + sample.us);
        lv_timer_handler();
        lv_refr_now(display.lv_display());
        if (presented != display.presentations) {
            // Only CPU-rendered UI pixels are copied, and only where LVGL
            // drew (the first frame is all of it). Video stays in DMA-BUFs.
            const auto damage = display.take_damage();
            overlay.sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            for (const auto &d : damage)
                for (int y = d.y; y < d.y + d.height; ++y)
                    memcpy(overlay.data + size_t(y) * overlay.stride + size_t(d.x) * 4,
                           display.pixels().data() + size_t(y) * source.width + d.x, size_t(d.width) * 4);
            overlay.sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            presented = display.presentations;
        }
        ar_ge2d_image input{};
        input.width = source.width;
        input.height = source.height;
        input.format = AR_GE2D_YUV420;
        for (int p = 0; p < 3; ++p) {
            input.fd[p] = frame->prime_fd;
            input.stride[p] = frame->pitches[p];
            input.offset[p] = frame->offsets[p];
        }
        compositor.compose(input, destination, {panel});
        frame.reset(); // decoder may reuse only after GE2D completes
        encoder.frame(destination, output.bytes, sample.us, sample.ticks);
        const int progress = uint64_t(i + 1) * 100 / source.count;
        if (progress != percent || std::chrono::steady_clock::now() - reported >= std::chrono::seconds(1)) {
            reported = std::chrono::steady_clock::now();
            percent = progress;
            printf("EXPORT PROGRESS %d\n", std::min(99, percent));
        }
    }
    require(had_hud, "Recording contains no usable HUD snapshots");
    require(!canceled, "Export canceled");
    encoder.finish();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    fprintf(stderr, "export: %u frames %.3fs %.2ffps; DMA decode/composition/encode; CPU video copies=0\n",
            source.count, seconds, source.count / seconds);
    puts("EXPORT DONE");
}
} // namespace
int main(int argc, char **argv) {
    // Dies with kestrel-gnd's export thread: an orphan would keep GE2D (one
    // opener at a time) and a decoder instance. Its partial file is removed
    // by the next ExportJob.
    const pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
        return 1;
    setpriority(PRIO_PROCESS, 0, 10);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGTERM, signal_stop);
    signal(SIGINT, signal_stop);
    lv_init();
    lv_tick_set_cb([] { return video_ms; });
    int result = 0;
    try {
        run(argc, argv);
    } catch (const std::exception &e) {
        printf("EXPORT ERROR %s\n", e.what());
        result = 1;
    }
    lv_deinit();
    return result;
}

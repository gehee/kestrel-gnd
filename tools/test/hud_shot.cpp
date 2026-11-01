// The HUD's screens drawn off the goggle: an OSD on a PC's GPU (a render
// node, no display), given parts of its own (a link, an air unit) and a
// picture, keys
// pressed as the buttons would, and each screen it draws written to OUTDIR as
// a PNG, with a digest of its pixels:
//
//   hud_shot OUTDIR [canopy | arena | arena_full | off] [render node]
//
// canopy (the default) takes the idle screen, the HUD, every tab of the
// menu and the channel page, and with HUD_SHOT_DVR the gallery; the other
// HUD styles the idle screen and the HUD. The render node is
// /dev/dri/renderD128 unless given.
//
// Not a ctest: it needs a GPU, and its pictures are this machine's (its
// fonts, its driver). What it is for is a change to how the HUD is drawn
// that should change nothing: the same digests before and after, on the
// same machine. The animations run on the clock, so each screen is taken
// once they have settled.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <cairo.h>
#include <GLES2/gl2.h>

#include "base/settings.hpp"
#include "display/drm.hpp"
#include "dvr/dvr_library.hpp"
#include "kestrel_gnd_config.h"
#include "platform/platform.hpp"
#include "radio/ar8030_sky.hpp"
#include "hud/osd.hpp"
#include "hud/parts.hpp"
#include "hud/settings.hpp"
#include "region/zones.hpp"
#include "render/gl_context.hpp"

// The version SYSTEM shows, the same in every build: the stamp's own would
// put a new build time on that tab each build (the library's is not linked
// in with these defined).
const char kestrel_gnd_version[]    = "test";
const char kestrel_gnd_build_time[] = "2026-01-01 00:00 UTC";
const char kestrel_gnd_git_hash[]   = "0000000";

namespace {

std::vector<uint8_t> g_pixels;   // the last frame, RGBA, top row first
int g_w = 0, g_h = 0;
bool g_want = false;

uint64_t fnv(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; }
    return h;
}

void write_png(const std::string& path) {
    // Over black, as the screen shows it with no picture behind: the frame is
    // premultiplied RGBA, so its colour is what it adds. cairo's RGB24 is
    // BGRx in memory on a little-endian machine.
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, g_w);
    std::vector<uint8_t> argb((size_t)stride * g_h);
    for (int y = 0; y < g_h; y++)
        for (int x = 0; x < g_w; x++) {
            const uint8_t* p = &g_pixels[((size_t)y * g_w + x) * 4];
            uint8_t* q = &argb[(size_t)y * stride + x * 4];
            q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = 255;
        }
    cairo_surface_t* s = cairo_image_surface_create_for_data(argb.data(), CAIRO_FORMAT_RGB24, g_w, g_h, stride);
    cairo_surface_write_to_png(s, path.c_str());
    cairo_surface_destroy(s);
}

// The parts the HUD shows (system/), as plain fakes: each holds the state it
// is given and wakes its watcher, and numbers what it is asked without
// carrying it out - nothing here is a radio. All on this one thread.
uint32_t g_seq = 0;
Ticket ask(int value) { return Ticket{++g_seq, value}; }

template <class I> class Fake : public I {
public:
    using S = decltype(std::declval<const I&>().state());
    S s{};
    S state() const override { return s; }
    uint32_t version() const override { return v_; }
    void watch(bus::Waker* w) override { w_ = w; }
    void publish() { v_++; if (w_) w_->poke(); }
protected:
    uint32_t v_ = 0;
    bus::Waker* w_ = nullptr;
};
struct FakeCamera : Fake<air::Camera> {
    Ticket set(air::CameraField, int v) override { return ask(v); }
    Ticket set_mode(int i) override { return ask(i); }
};
struct FakeEncoder : Fake<air::Encoder> {
    Ticket set_max_bitrate(int kbps) override { return ask(kbps); }
};
struct FakeAirRadio : Fake<air::Radio> {
    Ticket set_tx_power(int mw) override { return ask(mw); }
};
struct FakeUnit : Fake<air::Unit> {
    Ticket set_standby(bool on) override { return ask(on); }
    Ticket set_logs(bool on) override { return ask(on); }
};
struct FakeFc : air::Fc {
    std::deque<air::FcChunk> q;
    bus::Waker* w = nullptr;
    bool take(air::FcChunk& c) override {
        if (q.empty()) return false;
        c = q.front();
        q.pop_front();
        return true;
    }
    bool pending() const override { return !q.empty(); }
    uint64_t dropped() const override { return 0; }
    void watch(bus::Waker* x) override { w = x; }
    void push(const air::FcChunk& c) {
        const bool was_empty = q.empty();
        q.push_back(c);
        if (was_empty && w) w->poke();
    }
};
struct FakeRadio : Fake<gnd::Radio> {
    std::deque<gnd::ScanSample> scans;
    std::deque<float> rates;
    Ticket set_firmware(int firmware) override { return ask(firmware); }
    Ticket set_frame(int mode) override { return ask(mode); }
    Ticket set_max_mcs(int mcs) override { return ask(mcs); }
    Ticket set_max_bandwidth(int mhz) override { return ask(mhz); }
    Ticket set_steady(bool on) override { return ask(on); }
    Ticket set_channel(int) override { return ask(0); }
    void search_channels() override {}
    void scan_screen(bool) override {}
    void bind() override {}
    void set_dist_offset(int) override {}
    bool take_scan(gnd::ScanSample& sc) override { return take(scans, sc); }
    bool take_rate(float& mbps) override { return take(rates, mbps); }
    uint64_t dropped() const override { return 0; }
    void scan(const gnd::ScanSample& sc) { scans.push_back(sc); if (w_) w_->poke(); }
    void rate(float mbps) { rates.push_back(mbps); if (w_) w_->poke(); }
private:
    template <class T> static bool take(std::deque<T>& q, T& out) {
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
};
struct FakeTiming : Fake<gnd::Timing> {};

struct Rig {
    FakeCamera camera;
    FakeEncoder encoder;
    FakeAirRadio air_radio;
    FakeUnit unit;
    FakeFc fc;
    FakeRadio radio;
    FakeTiming timing;
    hud::Parts parts{camera, encoder, air_radio, unit, fc, radio, timing};
    gnd::RadioState& st = radio.s;
    air::UnitState& air = unit.s;
    OSD* osd = nullptr;
    std::string out;
    int shots = 0;

    void frames(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            osd->test_frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
    // Pictures arriving for ms (the renderer's calls), with the HUD drawing.
    void video(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        int n = 0;
        while (std::chrono::steady_clock::now() < end) {
            if (n % 60 == 0) radio.rate(st.video_mbps);   // the radio's, each second
            osd->notify_video_frame(n++ % 30 == 0);
            LatencyFrame lf{};
            lf.processing_ms = 4.0f; lf.net_ms = 9.0f; lf.dec_ms = 5.0f; lf.disp_ms = 8.0f;
            lf.pace_ms = 16.7f; lf.video_mbps = 18.0f; lf.rf_mbps = 24.0f;
            osd->add_latency_frame(lf);
            osd->test_frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
    void key(int k, int settle_ms = 400) {
        osd->handle_key(k);
        video(settle_ms);
    }
    void shot(const char* name) {
        video(1200);   // settled
        g_want = true;
        osd->test_frame();
        g_want = false;
        char path[512];
        snprintf(path, sizeof(path), "%s/%02d_%s.png", out.c_str(), shots++, name);
        write_png(path);
        printf("%016llx %s\n", (unsigned long long)fnv(g_pixels), name);
        fflush(stdout);
    }
    void publish() {
        radio.publish();
        unit.publish();
    }
    // The flight controller's screen, as MSP DisplayPort frames (MSP v1,
    // function 182) on the FC channel: strings at rows and columns, then
    // the draw that shows them.
    void fc_screen(const std::vector<std::pair<std::pair<int, int>, std::string>>& lines) {
        std::vector<uint8_t> bytes;
        auto frame = [&](const std::vector<uint8_t>& payload) {
            const uint8_t size = (uint8_t)payload.size(), cmd = 182;
            uint8_t ck = size ^ cmd;
            bytes.insert(bytes.end(), {'$', 'M', '>', size, cmd});
            for (uint8_t b : payload) { bytes.push_back(b); ck ^= b; }
            bytes.push_back(ck);
        };
        frame({2});                                  // clear
        for (const auto& l : lines) {
            std::vector<uint8_t> p = {3, (uint8_t)l.first.first, (uint8_t)l.first.second, 0};
            p.insert(p.end(), l.second.begin(), l.second.end());
            frame(p);
        }
        frame({4});                                  // draw
        for (size_t i = 0; i < bytes.size(); i += air::FcChunk::kMax) {
            air::FcChunk c;
            c.n = (uint16_t)std::min<size_t>(air::FcChunk::kMax, bytes.size() - i);
            memcpy(c.b, &bytes[i], c.n);
            fc.push(c);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s OUTDIR [canopy|arena|arena_full|off] [render node]\n", argv[0]);
        return 2;
    }
    const std::string style = argc > 2 ? argv[2] : "canopy";
    const char* node = argc > 3 ? argv[3] : "/dev/dri/renderD128";
    const int overlay = style == "off" ? 0 : style == "canopy" ? 1 : style == "arena" ? 2 : style == "arena_full" ? 3 : -1;
    if (overlay < 0) { fprintf(stderr, "unknown style %s\n", style.c_str()); return 2; }
    const int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(node); return 1; }

    // The settings it starts with: what the goggle has by default, but the
    // clock (which would differ between two runs), the background clip and
    // the HUD style asked for.
    // The idle screen's background, from HUD_SHOT_ASSETS (a background.png
    // of the caller's) - none on a PC otherwise.
    if (const char* assets = getenv("HUD_SHOT_ASSETS")) osd_set_assets_dir(assets);
    Settings settings;
    settings.set(hud::setting::clock_show, 0);
    settings.set(hud::setting::bg_video_enabled, false);
    settings.set(hud::setting::hud_overlay, overlay);
    zones::load(settings);

    // A 1080p screen with nothing behind it: the OSD's frames are drawn into
    // GBM buffers on the render node and never shown.
    auto dev = std::make_shared<DrmDevice>(settings);
    dev->drm_fd = fd;
    dev->output_list = new modeset_output();
    dev->output_list->mode.hdisplay = 1920;
    dev->output_list->mode.vdisplay = 1080;
    dev->output_list->mode.vrefresh = 60;
    if (!GlContext::instance().init(fd, false)) return 1;

    Rig r;
    r.out = argv[1];
    volatile bool stop = false;
    OSD osd(r.parts, settings, dev, platform::make_canvas(), platform::make_hud_layer(*dev), 1000, &stop, false);
    r.osd = &osd;
    osd.set_frame_hook([](int w, int h) {
        if (!g_want) return;
        g_w = w; g_h = h;
        std::vector<uint8_t> raw((size_t)w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        g_pixels.resize(raw.size());
        for (int y = 0; y < h; y++)   // bottom row first in GL
            memcpy(&g_pixels[(size_t)y * w * 4], &raw[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    });
    // The camera's modes, as main() gives them (the VIDEO tab).
    {
        std::vector<std::string> names;
        std::vector<uint16_t> needs;
        for (int i = 0; i < sky::kFpvModeCount; i++) {
            names.push_back(sky::kFpvModes[i].name);
            needs.push_back(sky::kFpvModes[i].need);
        }
        osd.set_video_mode_names(names, sky::default_fpv_mode_index(), needs);
    }
    osd.test_init();

    // No air unit yet: the idle screen.
    r.publish();
    r.frames(500);
    r.shot((style + "_idle").c_str());

    // An air unit linked, and its picture: the HUD over the video.
    r.st.link.state = gnd::kLinkUp;
    r.st.link.snr = 24.5f;
    r.st.link.mcs = 8;
    r.st.link.tx_freq = r.st.link.rx_freq = 5800;
    r.st.link.bw_step = 4;
    r.st.link.air_pwr_mw = 25;
    r.st.video_mbps = 18.0f;
    r.st.dist_m = 42;
    r.air.info.version = true;
    r.air.info.kestrel = true;
    r.air.vtx_temp_valid = true;
    r.air.vtx_cpu_c = 55.0f;
    r.st.rf_caps = gnd::RF_CAP_POWER | gnd::RF_CAP_MCS | gnd::RF_CAP_BW | gnd::RF_CAP_CHAN;
    r.publish();
    osd.set_video_resolution(1920, 1080);
    // The FC's own screen over it (HUD > Betaflight OSD).
    r.fc_screen({{{1, 2}, "ACRO"}, {{1, 45}, "16.4V"}, {{18, 2}, "00:42"}, {{18, 44}, "12.3A"}, {{9, 24}, "DISARMED"}});
    r.video(2500);
    r.shot((style + "_live").c_str());
    auto weak_link = [&] {
        // A weaker link: MCS 5 on 10 MHz, half the range left - the amber
        // edge and the VIDEO track's colour.
        r.st.link.rx_mcs_val = 7;
        r.st.link.bw_step = 3;
        r.publish();
        r.video(1500);
        r.shot((style + "_weak").c_str());
    };
    if (style != "canopy") {
        weak_link();
        stop = true;
        return 0;
    }

    // The menu: OK wakes the canopy, OK unfolds the blade picked, Left and
    // Right go between its tabs, Down walks a tab's rows, Back folds it.
    const int kUp = 0x101, kDown = 0x102, kRight = 0x103, kLeft = 0x104, kOk = 13, kBack = OSD::kKeyBack;
    r.key(kOk);
    r.shot("menu_awake");
    r.key(kOk);                     // on RADIO, the side's first open
    r.key(kLeft);
    for (int i = 0; i < 4; i++) {   // the air unit's: VIDEO, RADIO, SERIAL, INFO
        char name[32];
        snprintf(name, sizeof(name), "menu_air_tab%d", i);
        r.shot(name);
        r.key(kRight);
    }
    for (int i = 0; i < 2; i++) r.key(kLeft);   // RADIO
    for (int i = 0; i < 3; i++) r.key(kDown);
    r.shot("menu_radio_rows");
    for (int i = 0; i < 3; i++) r.key(kUp);
    // RADIO > Channel: the channel page, over a scan of the band.
    {
        gnd::ScanSample sc;
        sc.chan_num = 8;
        sc.work_chan = 3;
        for (int i = 0; i < sc.chan_num; i++) {
            sc.freq_mhz[i] = 5740 + 20 * i;
            sc.power_dbm[i] = -95 + 7 * (i % 4);
        }
        sc.last_update_ms = 1;
        r.radio.scan(sc);
    }
    r.key(kOk, 800);
    r.shot("channel_page");
    r.key(kBack);
    r.key(kBack);
    r.key(kLeft);
    r.shot("menu_ground_picked");
    r.key(kOk);
    for (int i = 0; i < 4; i++) {   // the goggle's: HUD, DISPLAY, DVR, SYSTEM
        char name[32];
        snprintf(name, sizeof(name), "menu_ground_tab%d", i);
        r.shot(name);
        r.key(kRight);
    }
    r.key(kBack);
    r.key(kBack);
    r.shot("live_after_menu");
    weak_link();

    // The gallery, given recordings of its own (HUD_SHOT_DVR: a folder of
    // recordings with their thumbnails, made by the caller): Back on the live
    // picture opens it on the live tile, Left goes to a recording, Right past
    // the live tile to the stats tile, OK opens the stats screen. The stats
    // tile and screen chart this run's timing, so those two are not the same
    // between runs; the others are.
    if (const char* dvr = getenv("HUD_SHOT_DVR")) {
        dvr_lib::set_dir(dvr);
        r.key(kBack, 1500);
        r.shot("gallery_live");
        r.key(kLeft, 1200);
        r.shot("gallery_recording");
        r.key(kRight, 1200);
        r.key(kRight, 1200);
        r.shot("gallery_stats_tile");
        r.key(kOk, 1500);
        r.shot("gallery_stats_screen");
        r.key(kBack, 1500);
        r.key(kBack, 1500);
        r.shot("gallery_closed");
    }

    stop = true;
    return 0;
}

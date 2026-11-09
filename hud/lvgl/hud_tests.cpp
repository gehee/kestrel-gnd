#include "controller.hpp"
#include "hud_source.hpp"
#include "hud_buffer.hpp"
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F f, const char* text) {
    bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } check(caught, text);
}
}
int main(int argc, char** argv) {
    using namespace kestrel_ui;
    using namespace kestrel_ui::hud_ipc;
    try {
        if (argc != 3 && argc != 4) throw std::runtime_error("Usage: hud-tests ASSETS HUD_EXECUTABLE [vector]");
        const bool vector = argc == 4 && std::string(argv[3]) == "vector";
        HudData data; data.status = "NO RF LINK"; data.panels[1].upper_label = "RC RSSI"; data.panels[1].upper_level = 0.45f;
        data.panels[0].recording = "REC 2:03";
        data.panels[1].detail1_tail = " • 88C"; data.panels[1].detail1_alert = 2;
        data.panels[1].lower_alert = 1;
        const auto wire = encode(data);
        check(encode(decode_data(wire)) == wire, "HUD schema round trip");
        auto bad_alert = wire; bad_alert.back() = 3;
        rejects([&] { decode_data(bad_alert); }, "Unknown alert rejected");
        auto short_wire = wire; short_wire.pop_back(); rejects([&] { decode_data(short_wire); }, "Truncated fields rejected");
        auto extra_wire = wire; extra_wire.push_back(0); rejects([&] { decode_data(extra_wire); }, "Trailing fields rejected");
        auto bad_flag = wire; bad_flag[0] = 2; rejects([&] { decode_data(bad_flag); }, "Non-boolean flags rejected");
        data.panels[0].primary.assign(129, 'x'); rejects([&] { encode(data); }, "Long strings rejected");
        data.panels[0].primary = "--"; data.panels[0].dim = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { encode(data); }, "NaN presentation values rejected");
        HudData metrics;
        metrics.metrics.push_back({HudMetricId::Rc, true, true, .45f, "RC RSSI", "45", "%", {"0", "25", "50", "75", "100"}});
        check(encode(decode_data(encode(metrics))) == encode(metrics), "Display-ready metric round trip");
        metrics.metrics.push_back(metrics.metrics.front());
        rejects([&] { encode(metrics); }, "Duplicate metric IDs rejected");
        Layout layout{1920, 1080, {{0, 720, 960, 360, Role::LeftWing}}};
        check(decode_layout(encode(layout)) == layout, "Surface layout round trip");
        auto bad_layout = layout; bad_layout.regions[0].width = 2000;
        rejects([&] { bad_layout.validate(); }, "Out-of-viewport regions rejected");
        bad_layout = layout; bad_layout.regions.assign(9, layout.regions[0]);
        rejects([&] { bad_layout.validate(); }, "Excessive region counts rejected");
        int sockets[2]; check(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets), "Socket pair");
        {
            HudBuffer buffer(layout);
            check(send(sockets[0], Type::Setup, 0, encode(layout), buffer.fd()), "Descriptor transfer");
            Packet packet; check(receive(sockets[1], packet) && packet.type == Type::Setup && packet.fd >= 0, "Setup received");
            check(fcntl(packet.fd, F_GETFD) & FD_CLOEXEC, "Received descriptor is close-on-exec");
            const int fd = packet.fd; packet.fd = -1;
            HudBuffer peer(decode_layout(packet.payload), fd);
            buffer.region(1, 0)[17] = 123;
            check(peer.region(1, 0)[17] == 123, "Shared pixels are shared, not socket-copied");
            rejects([&] { peer.region(2, 0); }, "Invalid buffer slot rejected");
        }
        const uint8_t bad_header[16]{};
        check(::send(sockets[0], bad_header, sizeof(bad_header), 0) == sizeof(bad_header), "Malformed test packet sent");
        rejects([&] { Packet packet; receive(sockets[1], packet); }, "Unknown protocol header rejected");
        close(sockets[0]); close(sockets[1]);

        lv_init();
        {
            auto* display = lv_display_create(1920, 1080); lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
            {
                Model model; Controller controller(model);
                uint64_t now = monotonic_us();
                model.backend.vrx.stamp_us = now; model.backend.vrx.connected = true; model.backend.vrx.mcs = 10;
                model.backend.vrx.voltage_mv = 11512; model.backend.vrx.cpu_temperature_x100 = 5800;
                auto& f = model.backend.flight; f.stamp_us = now; f.have_pack_v = true; f.pack_v = 11.8f;
                f.have_amps = true; f.amps = 0.1f; f.have_rssi = true; f.rssi_pct = 45; f.arm = 0;
                model.visual.visible = true; model.visual.rule = model.visual.shade = model.visual.block = 1;
                InlineHud reference(1920, 1080, argv[1], vector ? HudStyle::Vector : HudStyle::Canopy); HudClient worker(1920, 1080, argv[1], argv[2]);
                auto compare = [&](const HudData& given) {
                    reference.update(given, monotonic_us());
                    std::unique_ptr<InlineHud> cold;
                    if (vector) {
                        cold = std::make_unique<InlineHud>(1920, 1080, argv[1], HudStyle::Vector);
                        cold->update(given, monotonic_us());
                    }
                    const auto before = worker.frames(); const uint64_t deadline = monotonic_us() + 4000000;
                    do {
                        worker.update(given, monotonic_us());
                        if (worker.frames() > before) break;
                        usleep(2000);
                    } while (monotonic_us() < deadline);
                    check(worker.frames() > before, "Worker returned a frame");
                    check(worker.layout() == reference.layout(), "Worker surface bounds match reference");
                    for (size_t i = 0; i < reference.layout().regions.size(); ++i) {
                        const auto* a = reference.image(i); const auto* b = worker.image(i);
                        check(a && b && a->data_size == b->data_size && !std::memcmp(a->data, b->data, a->data_size),
                              "Shared worker frame differs from synchronous renderer");
                        if (cold) check(!std::memcmp(a->data, cold->image(i)->data, a->data_size),
                                        "Cached Vector regions differ from a fresh render");
                    }
                    check(worker.restarts() == 0, "Normal frames require no renderer restart");
                };
                compare(controller.hud_data(now));
                f.rssi_pct = 0; compare(controller.hud_data(now));
                const uint32_t rc_damage = vector ? (1u << 2) | (1u << 4) : 1u << 1;
                check(worker.changed_regions() == rc_damage && reference.changed_regions() == rc_damage,
                      "RSSI updates damage only RC instrument regions");
                controller.set_scene(SceneMode::GalleryBrowse); compare(controller.hud_data(now));
                for (size_t i = 0; i < worker.layout().regions.size(); ++i) {
                    const auto* image = worker.image(i);
                    for (size_t j = 0; j < image->data_size; ++j) check(image->data[j] == 0, "Gallery HUD surfaces are transparent");
                }
                const auto hidden_frames = worker.frames();
                controller.set_scene(SceneMode::GalleryPlayback); f.pack_v = 11.5f;
                for (int i = 0; i < 8; ++i) { worker.update(controller.hud_data(now), monotonic_us()); usleep(2000); }
                check(worker.frames() == hidden_frames, "Hidden telemetry changes do not trigger HUD drawing");
                controller.set_scene(SceneMode::Live); f.rssi_pct = 80; compare(controller.hud_data(now));
                // Kill only the passive renderer. State and graphics host survive.
                const int old_pid = worker.pid(); kill(old_pid, SIGKILL);
                const uint64_t deadline = monotonic_us() + 5000000;
                const auto before = worker.frames();
                do { worker.update(controller.hud_data(now), monotonic_us()); usleep(2000); }
                while ((worker.pid() == old_pid || worker.frames() == before) && monotonic_us() < deadline);
                check(worker.pid() > 0 && worker.pid() != old_pid && worker.frames() > before && worker.restarts() == 1,
                      "Killed HUD restarts and receives current data");
                check(!model.menu_open && f.rssi_pct == 80, "Renderer restart cannot change UI/telemetry state");
            }
            lv_display_delete(display);
        }
        lv_deinit();
        std::puts("PASS: strict schema, fd/shared buffers, exact worker pixels, gallery pause and crash/resync");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}

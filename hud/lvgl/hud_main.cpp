#include "hud_renderer.hpp"
#include "lvgl.h"
#include "clock.hpp"
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace {
volatile sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}
int main(int argc, char** argv) {
    try {
        int ipc = -1, width = 0, height = 0; std::string assets;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i]; if (++i == argc) throw std::runtime_error("Missing HUD option");
            if (key == "--ipc-fd") ipc = std::stoi(argv[i]);
            else if (key == "--width") width = std::stoi(argv[i]);
            else if (key == "--height") height = std::stoi(argv[i]);
            else if (key == "--assets") assets = argv[i];
            else throw std::runtime_error("Unknown HUD option");
        }
        if (ipc < 0 || width < 640 || width > 4096 || height < 360 || height > 2160 || assets.empty())
            throw std::runtime_error("Invalid HUD setup arguments");
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        struct sigaction action{}; action.sa_handler = stop; sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr); sigaction(SIGINT, &action, nullptr);
        lv_init();
        {
            // Snapshot rendering needs a display context, but no framebuffer,
            // video API, input source, backend or full-screen drawing buffer.
            auto* display = lv_display_create(width, height);
            if (!display) throw std::runtime_error("Cannot create HUD render context");
            lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
            {
                #ifdef KESTREL_VECTOR_HUD
                constexpr auto style = kestrel_ui::HudStyle::Vector;
                #else
                constexpr auto style = kestrel_ui::HudStyle::Canopy;
                #endif
                auto renderer = kestrel_ui::make_hud_renderer(style, width, height, assets);
                auto& view = *renderer;
                using namespace kestrel_ui::hud_ipc;
                while (!stopped && !send(ipc, Type::Hello, 0, encode(view.layout()))) { pollfd p{ipc, POLLOUT, 0}; ::poll(&p, 1, 100); }
                std::unique_ptr<kestrel_ui::HudBuffer> buffer;
                std::optional<kestrel_ui::HudData> latest;
                uint64_t latest_seq = 0, rendered_seq = 0;
                int last_slot = -1;
                std::array<uint64_t, 2> leases{};
                while (!stopped) {
                    for (int i = 0; i < 16; ++i) {
                        Packet p; if (!receive(ipc, p)) break;
                        if (p.type == Type::Setup && !buffer && p.fd >= 0 && p.sequence == 0) {
                            const auto layout = decode_layout(p.payload);
                            if (!(layout == view.layout())) throw std::runtime_error("HUD setup layout mismatch");
                            const int fd = p.fd; p.fd = -1;
                            buffer = std::make_unique<kestrel_ui::HudBuffer>(layout, fd);
                        } else if (p.type == Type::Data && buffer && p.fd < 0 && p.sequence > latest_seq) {
                            latest = decode_data(p.payload); latest_seq = p.sequence;
                        } else if (p.type == Type::Release && buffer && p.fd < 0) {
                            const auto f = decode_frame(p.payload);
                            if (!leases[f.slot] || leases[f.slot] != p.sequence) throw std::runtime_error("Invalid HUD buffer release");
                            leases[f.slot] = 0;
                        } else throw std::runtime_error("Unexpected HUD controller message");
                    }
                    int slot = leases[0] == 0 ? 0 : leases[1] == 0 ? 1 : -1;
                    if (latest && latest_seq > rendered_seq && slot >= 0) {
                        const uint64_t started = kestrel_ui::monotonic_us();
                        view.render(*latest, *buffer, uint32_t(slot));
                        Frame f{uint32_t(slot), kestrel_ui::monotonic_us() - started, view.cache_bytes(), view.rasters()};
                        for (size_t i = 0; i < view.layout().regions.size(); ++i)
                            if (last_slot < 0 || std::memcmp(buffer->region(uint32_t(last_slot), i), buffer->region(uint32_t(slot), i), view.layout().regions[i].bytes()))
                                f.changed_regions |= 1u << i;
                        while (!stopped && !send(ipc, Type::Frame, latest_seq, encode(f))) { pollfd p{ipc, POLLOUT, 0}; ::poll(&p, 1, 100); }
                        leases[slot] = latest_seq; rendered_seq = latest_seq; last_slot = slot;
                    }
                    pollfd p{ipc, POLLIN, 0}; ::poll(&p, 1, 20);
                }
            }
            lv_display_delete(display);
        }
        lv_deinit(); close(ipc); return 0;
    } catch (const std::exception& e) {
        if (!stopped) std::fprintf(stderr, "kestrel-hud: %s\n", e.what());
        return stopped ? 0 : 1;
    }
}

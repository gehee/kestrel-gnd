#include "display.hpp"
#include "view.hpp"
#include "controller.hpp"
#include "render/software/frame_cadence.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

namespace {
uint32_t tick() { return uint32_t(kestrel_ui::monotonic_us() / 1000); }
double cpu_us() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000000.0 + u.ru_utime.tv_usec + u.ru_stime.tv_usec;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("Usage: kestrel-ui-animation-bench ASSETS FPS DRAW_ROWS timer|paced");
        const int fps = std::stoi(argv[2]), rows = std::stoi(argv[3]);
        const std::string mode = argv[4];
        const bool paced = mode == "paced";
        if (fps < 1 || fps > 60 || (mode != "timer" && !paced)) throw std::runtime_error("Invalid benchmark options");
        lv_init(); lv_tick_set_cb(tick);
        {
            // No framebuffer, radio/control socket, media API or vendor files:
            // this is an offscreen rendering lower bound, not an HDMI/vsync test.
            kestrel_ui::Display display(1920, 1080, "", rows);
            lv_timer_set_period(lv_display_get_refr_timer(display.lv_display()), 1000 / fps);
            kestrel_ui::Model model;
            kestrel_ui::Controller controller(model);
            model.menu_open = true; model.side = 0; model.tab = 1;
            kestrel_ui::View view(controller, 1920, 1080, argv[1]);
            const uint64_t started = kestrel_ui::monotonic_us();
            uint64_t next = started;
            const double cpu_start = cpu_us();
            uint64_t animated = 0, painted = 0, missed = 0;
            std::vector<uint64_t> costs;
            while (kestrel_ui::monotonic_us() - started < 6000000) {
                const uint64_t now = kestrel_ui::monotonic_us();
                if (now < next) { usleep(unsigned(std::min<uint64_t>(2000, next - now))); continue; }
                model.backend.vrx.stamp_us = now; model.backend.vrx.connected = true; model.backend.vrx.mcs = 10;
                model.backend.vrx.voltage_mv = 11400; model.backend.vrx.cpu_temperature_x100 = 5800;
                const auto stage = ((now - started) / 360000) % 2 ? kestrel_ui::MenuStage::Pick : kestrel_ui::MenuStage::Browse;
                if (model.stage != stage) { model.stage = stage; ++model.menu_version; }
                const uint64_t before = display.presentations;
                controller.tick(now); view.update(now); lv_timer_handler();
                if (paced) lv_refr_now(display.lv_display());
                const uint64_t finished = kestrel_ui::monotonic_us();
                if (now - started > 1000000 && view.animating()) {
                    ++animated; if (display.presentations != before) ++painted;
                    costs.push_back(finished - now);
                }
                const uint64_t period = 1000000 / fps;
                if (paced) {
                    const uint64_t following = kestrel_ui::next_frame_deadline(next, finished, period);
                    missed += (following - next) / period - 1;
                    next = following;
                } else next = now + period;
            }
            const double wall = double(kestrel_ui::monotonic_us() - started);
            const double cpu = (cpu_us() - cpu_start) / wall * 100;
            std::sort(costs.begin(), costs.end());
            uint64_t sum = 0; for (auto us : costs) sum += us;
            rusage usage{}; getrusage(RUSAGE_SELF, &usage);
            std::printf("BENCH mode=%s fps=%d rows=%d animated_ticks=%llu painted=%llu avg_us=%llu p95_us=%llu max_us=%llu skipped_deadlines=%llu cpu_one_core_pct=%.2f rss_kib=%ld text_bytes=%zu snapshots=%llu\n",
                        mode.c_str(), fps, rows, (unsigned long long)animated, (unsigned long long)painted,
                        (unsigned long long)(costs.empty() ? 0 : sum / costs.size()),
                        (unsigned long long)(costs.empty() ? 0 : costs[(costs.size() - 1) * 95 / 100]),
                        (unsigned long long)(costs.empty() ? 0 : costs.back()), (unsigned long long)missed,
                         cpu, usage.ru_maxrss, view.text_cache_bytes(), (unsigned long long)view.menu_snapshots());
        }
        lv_deinit(); return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}

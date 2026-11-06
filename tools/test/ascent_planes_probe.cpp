// On-device integration probe. Real Wave5 decode + two KMS planes; no UI ticks
// during the measurement interval. Run only when no receiver owns the display.
#include "display/ar9311_screen.hpp"
#include "hal/v4l2/vdec_v4l2.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
#include <unistd.h>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ascent-planes-probe HEVC_1080P_ANNEXB\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot read HEVC clip");
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
        struct Nal {
            size_t begin, end;
            unsigned type;
        };
        std::vector<Nal> nals;
        for (size_t i = 0; i + 4 < data.size(); ++i) {
            unsigned prefix = 0;
            if (data[i] == 0 && data[i + 1] == 0) {
                if (data[i + 2] == 1)
                    prefix = 3;
                else if (data[i + 2] == 0 && data[i + 3] == 1)
                    prefix = 4;
            }
            if (prefix) {
                if (!nals.empty())
                    nals.back().end = i;
                nals.push_back({i, data.size(), unsigned((data[i + prefix] >> 1) & 63)});
                i += prefix - 1;
            }
        }
        if (nals.empty())
            throw std::runtime_error("no Annex-B NALs");
        volatile bool stop = false;
        auto screen = display::make_ar9311_screen("/dev/dri/card0", true, &stop);
        VdecV4l2 decoder(VideoCodec::H265, "/dev/video0", screen, &stop);
        struct Workers {
            volatile bool &stop;
            std::thread display, decode, feed;
            ~Workers() { finish(); }
            void finish() {
                stop = true;
                if (feed.joinable())
                    feed.join();
                if (decode.joinable())
                    decode.join();
                if (display.joinable())
                    display.join();
            }
        } workers{stop, {}, {}, {}};
        workers.display = std::thread([&] { screen->run(); });
        workers.decode = std::thread([&] { decoder.run_frame(); });
        workers.feed = std::thread([&] {
            uint64_t pts = 1;
            auto next = std::chrono::steady_clock::now();
            while (!stop && !decoder.failed() && !screen->failed()) {
                for (auto n : nals) {
                    if (stop)
                        break;
                    const bool vcl = n.type <= 31;
                    decoder.feed_packet_to_decoder(data.data() + n.begin, n.end - n.begin, pts, 0, n.type, 0,
                                                   0, n.type >= 16 && n.type <= 21);
                    if (vcl) {
                        ++pts;
                        next += std::chrono::microseconds(16667);
                        std::this_thread::sleep_until(next);
                    }
                }
            }
        });
        std::vector<uint32_t> ui(1920 * 1080, 0);
        for (unsigned y = 200; y < 600; ++y)
            for (unsigned x = 100; x < 1700; ++x) {
                // Four 400px regions: opaque red, half green, transparent blue,
                // opaque white. Alpha zero must preserve the decoded picture.
                const uint32_t colors[] = {0xffff0000, 0x8000ff00, 0x000000ff, 0xffffffff};
                ui[y * 1920 + x] = colors[(x - 100) / 400];
            }
        screen->overlay(ui.data(), 1920, 1080);
        sleep(3);
        const auto before = screen->presented();
        const auto started = std::chrono::steady_clock::now();
        puts("UI frozen for 10 seconds; Wave5 and video scanout continue independently");
        sleep(10);
        const auto frames = screen->presented() - before;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        printf("INDEPENDENCE: %llu display updates in %.3fs = %.2f/s, one UI upload\n",
               (unsigned long long)frames, seconds, frames / seconds);
        workers.finish();
        if (decoder.failed() || screen->failed() || frames < 100)
            throw std::runtime_error("independent decode/scanout failed");
        puts("PASS: hardware decode and independent scanout with stationary alpha overlay");
    } catch (const std::exception &e) {
        fprintf(stderr, "probe: %s\n", e.what());
        return 1;
    }
}

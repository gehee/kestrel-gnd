#include <atomic>
#include "hal/v4l2/vdec_v4l2.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <thread>
#include <chrono>

static void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
int main() {
    try {
        std::atomic<bool> stop = false;
        // No decode worker/device: emulate a radio read delivering a burst
        // before the consumer gets scheduled, then test bounded recovery.
        VdecV4l2 decoder(VideoCodec::H265, "", {}, &stop);
        uint8_t data[] = {0, 0, 0, 1, 0x26, 1, 0x80};
        auto feed = [&](uint8_t type) {
            decoder.feed_packet_to_decoder(data, sizeof(data), 1, 0, type, 0, 0);
        };
        for (uint8_t type : {32, 33, 34}) feed(type);
        feed(1);
        check(!decoder.keyframe_in(), "Predictive video cannot start without a keyframe");
        const auto initial_drops = decoder.dropped();
        feed(19);
        for (int i = 0; i < 47; ++i) feed(1);
        check(decoder.keyframe_in() && decoder.dropped() == initial_drops,
              "A short SDIO burst discarded the prediction chain");
        for (int i = 0; i < 256; ++i) feed(1);
        check(!decoder.keyframe_in() && decoder.dropped() > initial_drops,
              "An unconsumed backlog must remain bounded and request recovery");
        feed(1);
        check(!decoder.keyframe_in(), "A predictive picture bypassed keyframe recovery");
        feed(19);
        check(decoder.keyframe_in(), "A new keyframe did not recover after overflow");
        // A few unusually large access units must also obey the byte budget.
        std::vector<uint8_t> large(3 * 1024 * 1024);
        for (int i = 0; i < 3; ++i)
            decoder.feed_packet_to_decoder(large.data(), large.size(), 1, 0, 1, 0, 0);
        check(!decoder.keyframe_in(), "Compressed backlog exceeded its byte budget");
        feed(19);
        check(decoder.keyframe_in(), "Byte overflow did not recover on a keyframe");
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        feed(1);
        check(!decoder.keyframe_in(), "Stale unconsumed video must request a fresh keyframe");
        feed(19);
        check(decoder.keyframe_in(), "Age limit did not recover on a keyframe");
        puts("PASS: SDIO burst tolerance, bounded backlog and keyframe recovery");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}

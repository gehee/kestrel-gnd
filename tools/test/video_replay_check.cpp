// A recorded AR8030 video stream (what --debug-replay plays, e.g.
// test-streams/ar8030-live-1080p100-healthy.bin) through the radio's whole
// video path - Ar8030Link's splitter, the air unit's headers, the picture
// assembly, the parameter sets - into a fake decoder and a recording video
// sink, once, and a digest of everything handed out: each picture, slice and
// parameter set with its pts and type. Run before and after a change to that
// path, the two digests say whether it changed anything.
//   video_replay_check FILE        (builds with -DKESTREL_TESTS=ON, the PC HAL)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include "hal/vdec.hpp"
#include "radio/ar8030_config.hpp"
#include "radio/ar8030_link.hpp"
#include "radio/model.hpp"

namespace {

struct Digest {
    uint64_t h = 1469598103934665603ull;
    void mix(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    }
    template <class T> void val(const T& v) { mix(&v, sizeof(v)); }
};

// The decoder as the radio drives it: streamed pictures on, every call kept
// in the digest (but no arrival times: those are the wall clock's).
struct FakeVdec : Vdec {
    Digest d;
    uint64_t whole = 0, starts = 0, appends = 0, ends = 0, bytes = 0;
    void feed_packet_to_decoder(void* p, int n, int64_t pts, uint64_t, uint8_t type, uint32_t, uint32_t, bool) override {
        whole++; bytes += (uint64_t)n;
        d.val('W'); d.mix(p, (size_t)n); d.val(pts); d.val(type);
    }
    bool stream_supported() const override { return true; }
    void stream_start(void* p, int n, int64_t pts, uint64_t, uint8_t type, uint32_t, uint32_t, int slices) override {
        starts++; bytes += (uint64_t)n;
        d.val('S'); d.mix(p, (size_t)n); d.val(pts); d.val(type); d.val(slices);
    }
    bool stream_append(const void* p, int n, int64_t pts, bool last) override {
        appends++; bytes += (uint64_t)n;
        d.val('A'); d.mix(p, (size_t)n); d.val(pts); d.val(last);
        return true;
    }
    void stream_end(int64_t pts) override { ends++; d.val('E'); d.val(pts); }
    void cleanup() override {}
    void run_frame() override {}
};

// A recorder's end: every whole picture and parameter-set change.
struct Recorder : bus::VideoSink {
    Digest d;
    std::atomic<uint64_t> pictures{0};
    uint64_t keyframes = 0, bytes = 0, psets = 0;
    uint64_t whole[3] = {0, 0, 0};   // not said, short, whole (PictureBuf::whole + 1)
    void on_picture(const bus::PictureRef& p) override {
        keyframes += p->keyframe ? 1 : 0;
        whole[p->whole + 1]++;
        bytes += p->len;
        d.val('P'); d.mix(p->data(), p->len); d.val(p->pts); d.val(p->keyframe);
        pictures.fetch_add(1);
    }
    void on_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) override {
        psets++;
        d.val('Q');
        for (const auto& q : ps) d.mix(q.data(), q.size());
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    Settings settings;   // no file: nothing is written
    radio::Model model(settings);
    Ar8030Config cfg;
    cfg.replay_path = argv[1];
    cfg.decode_enabled = true;
    cfg.stream_decode = true;
    auto vdec = std::make_shared<FakeVdec>();
    std::atomic<bool> stop{false};
    Recorder rec;
    const VideoCodec codec = VideoCodec::H265;   // the link keeps a reference to it
    auto link = std::make_unique<Ar8030Link>(model, cfg, settings, codec, vdec, &stop);
    link->add_video_sink(&rec);
    std::thread radio([&] { link->run(); });
    // One pass: the replay rests 2 s between passes; a second without a new
    // picture after the first ones is that rest.
    uint64_t seen = 0;
    auto still_since = std::chrono::steady_clock::now();
    const auto give_up = still_since + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint64_t n = rec.pictures.load();
        if (n != seen) { seen = n; still_since = std::chrono::steady_clock::now(); continue; }
        if (n && std::chrono::steady_clock::now() - still_since > std::chrono::seconds(1)) break;
    }
    stop = true;
    radio.join();
    link.reset();
    printf("decoder: %llu whole, %llu streamed (%llu appends, %llu ended short), %llu bytes, digest %016llx\n",
           (unsigned long long)vdec->whole, (unsigned long long)vdec->starts,
           (unsigned long long)vdec->appends, (unsigned long long)vdec->ends,
           (unsigned long long)vdec->bytes, (unsigned long long)vdec->d.h);
    printf("recorder: %llu pictures (%llu keyframes), %llu bytes, %llu parameter-set changes, digest %016llx\n",
           (unsigned long long)rec.pictures.load(), (unsigned long long)rec.keyframes,
           (unsigned long long)rec.bytes, (unsigned long long)rec.psets, (unsigned long long)rec.d.h);
    printf("recorder: by the radio's slice numbers %llu whole, %llu short, %llu not said\n",
           (unsigned long long)rec.whole[2], (unsigned long long)rec.whole[1], (unsigned long long)rec.whole[0]);
    return 0;
}

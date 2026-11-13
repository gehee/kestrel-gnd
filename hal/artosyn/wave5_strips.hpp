#pragma once
// Strips on the AR9311 receivers, decoded in place: each strip's stream has a
// Wave5 decoder instance of its own, and every instance writes its rows
// straight into one whole-picture buffer (fpvOS kernel patch 0037: the
// display planes at offsets of ours) - no copy between the decoders and the
// screen, which takes the picture once its last strip is written.
//
// Each picture gets a buffer before its first strip is decoded, and each
// strip's decoder is given exactly that buffer before it is given the strip:
// with the offsets set the driver decodes with one capture buffer queued (the
// references stay in its compressed buffers), and the firmware cannot write
// a picture anywhere else. So the strips of a picture always share a buffer,
// whatever a decoder lost; a strip missing costs its picture only. One strip
// in a decoder at a time (the firmware picks among several free buffers by
// its own order, not the queue's) - a strip decodes in about 3.5 ms, a
// picture's strips come 16 ms apart.
//
// Memory: the codec has memory of its own (a reserved region, apart from the
// receiver's RAM and from CMA, which the screen's planes all but fill), too
// little for a second whole-picture decoder beside the strips'. While strips
// run the whole-picture decoder lets go of the hardware and its buffers
// (MainDecoder) and the strips' decoders and whole-picture buffers - the
// compressed buffers of a decoder instance, exported - take their place; it
// gets them back when the air unit sends whole pictures again (stopped()).
//
// One thread drives every strip decoder; the radio thread only queues
// packets (decoder(k)'s Vdec) and says what each strip's header said.
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "base/video_codec.hpp"
#include "hal/strip_joiner.hpp"
#include "hal/vdec.hpp"

namespace hal {

// Where each strip's decoded rows go in the whole picture's buffer. Strip k
// is visible[k] rows (coded[k] written, its coded height), codes ov rows past
// each of its joins, and its shown rows stack under the strips above it: its
// row 0 is written at origin[k], ov rows above its first shown row (but strip
// 0's). Where two strips' written rows overlap, the one written last stays
// (the lower, normally: it comes later). The planes are hp rows tall, enough
// for every strip's coded rows; the picture shown is w x h.
struct StripLayout {
    static constexpr int kMax = 4;
    int n = 0;
    uint32_t w = 0, h = 0, hp = 0;
    uint32_t origin[kMax] = {};
    uint32_t off[kMax][3] = {};   // Y, Cb, Cr byte offsets of strip k's planes
    uint32_t plane[3] = {};       // the whole picture's Y, Cb, Cr offsets
    size_t bytes() const { return (size_t)w * hp * 3 / 2; }
    // false where the strips cannot share a buffer (odd rows, a width the
    // planes' 32-byte alignment does not allow, nothing shown).
    static bool make(int n, int ov, uint32_t w, const uint32_t visible[], const uint32_t coded[], StripLayout* out);
};

class Wave5Strips final : public StripJoiner {
public:
    static constexpr int kMax = StripLayout::kMax;
    struct MainDecoder {
        std::function<void(bool)> hold;   // let go of the hardware and its buffers (true), or take them again
        std::function<bool()> released;   // has let go
    };
    // device: the decoder's node; out: where whole pictures go.
    Wave5Strips(VideoCodec codec, std::string device, std::shared_ptr<FrameSink> out, MainDecoder main);
    ~Wave5Strips() override;
    // Whether the decoder driver has the display offsets (patch 0037).
    static bool available(const std::string& device);

    std::unique_ptr<Vdec> decoder(int k) override;
    void arriving(int k, int64_t pic, bool last, int ov) override;
    void strip(int, bool, int, std::shared_ptr<DecodedUnit>) override {}   // joined in place: never handed one
    void drop() override;
    void stopped() override;

    // decoder(k)'s, from the radio thread.
    void feed(int k, const uint8_t* data, int len, int64_t pts, uint64_t recv, uint8_t nal, uint32_t enc,
              uint32_t proc, bool key, SliceTimesPtr times);
    bool keyframe_in(int k) const;
    uint64_t last_frame_us() const { return last_frame_us_.load(); }

private:
    struct Packet {
        std::vector<uint8_t> bytes;
        int64_t pts = 0;
        timing_stats timing{};
    };
    // A strip's stream as it comes (the radio thread's, under mtx_).
    struct In {
        std::deque<Packet> packets;
        std::array<std::vector<uint8_t>, 3> params;
        bool ready = false;   // a keyframe in: what follows decodes
    };
    // A strip's decoder (the thread's).
    struct Dev {
        int fd = -1;
        void* out = nullptr;
        size_t out_size = 0;
        bool out_busy = false, out_on = false, cap_on = false;
        bool src = false;      // its stream's size known (the source change)
        bool registered = false;  // its first picture out: the firmware has the buffers
        bool exact = false;       // and drained: a buffer of ours before each strip
        unsigned boot_fed = 0, boot_out = 0;   // strips in and out before that
        uint64_t first_fed_us = 0;  // its first strip in
        uint64_t boot_us = 0;     // the capture stream started, or its last picture before exact
        uint32_t width = 0, visible = 0, coded = 0;
        int min_bufs = 0;
        int64_t waiting = -1;  // a strip waiting for a buffer (counted once)
        int64_t flight = -1;   // the strip being decoded, and its buffer
        int flight_slot = -1;
        uint64_t flight_us = 0;
        timing_stats timing{};
    };
    // A whole-picture buffer.
    struct Slot {
        int fd = -1;
        uint8_t* map = nullptr;
        std::shared_ptr<int> owner;   // the pictures from it hold this (frame_ref): busy while they do
        int64_t pic = -1;             // the picture being written into it; -1: none
        unsigned queued = 0;          // strips whose decoder has it now
        unsigned got = 0;             // strips written
        bool given_up = false;        // never to be shown (a strip lost or too late)
        timing_stats t[kMax]{};
        uint64_t done_us[kMax] = {};
    };
    struct Said {
        int64_t pic = -1;
        bool last = false;
        int ov = 0;
    };

    void run();
    void begin();
    void teardown(bool for_good, bool exiting = false);
    bool open_dev(int k);
    void close_dev(Dev& d);
    void boot_feed(int k);
    void boot_drained(int k, uint64_t now);
    bool exact_feed(int k);
    void source_change(int k);
    bool setup();
    bool alloc_slots(int count);
    void free_slots();
    void service(int k);
    void captured(int k, unsigned index, int64_t pic, bool error);
    void finish(Slot& s);
    void retire_before(int64_t pic);
    int slot_for(int64_t pic, int k);
    bool slot_free(const Slot& s) const { return s.pic < 0 && !s.queued && s.owner && s.owner.use_count() == 1; }
    void wake();
    void log_stats();

    const VideoCodec codec_;
    const std::string device_;
    std::shared_ptr<FrameSink> out_;
    MainDecoder main_;

    mutable std::mutex mtx_;      // In, Said and the requests below
    In in_[kMax];
    Said said_[kMax][32];
    int seen_n_ = 0, seen_ov_ = 0;
    bool stop_req_ = false, drop_req_ = false, quit_ = false;
    int wake_fd_ = -1;

    // The thread's.
    std::thread thread_;
    bool active_ = false;          // strips running: the whole-picture decoder held
    bool flushed_ = false;         // the screen let go of the whole-picture decoder's pictures
    uint64_t begin_us_ = 0, retry_after_us_ = 0, alloc_since_us_ = 0;
    int n_ = 0, ov_ = 0;           // the layout set up for
    Dev dev_[kMax];
    std::vector<Slot> slot_;
    int slot_count_ = 0;
    StripLayout layout_;
    int alloc_fd_ = -1;            // the decoder instance whose compressed buffers they are
    uint32_t epoch_ = 0;
    bool clean_ = false;           // a whole picture shown since the start
    int64_t newest_ = -1;          // the newest picture shown
    std::atomic<uint64_t> last_frame_us_{0};
    unsigned long joined_ = 0, incomplete_ = 0, waits_ = 0, decodes_ = 0, errors_ = 0;
    // This thread's loops in 5 s, and what woke each: a packet, or decoder k
    // (its poll events, ORed): a busy loop shows as loops far above ~2 per strip.
    unsigned long loops_ = 0, woke_[kMax + 1] = {};
    short woke_ev_[kMax + 1] = {};
    uint64_t decode_us_ = 0, log_us_ = 0;
};

}  // namespace hal

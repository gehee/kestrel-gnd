#include "dvr/dvr_sink.hpp"

#include <pthread.h>

#include <cstdio>
#include <ctime>



static uint64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

// The radio's pool keeps this many buffers free for the pictures still to
// come (as DvrRecorder::feed does): below it, a queued reference would starve
// them, so the picture is copied.
static constexpr int kPoolReserve = 8;

DvrSink::DvrSink(VideoCodec codec, RecordingTarget& target, std::function<bool()> screen_rec)
    : codec_(codec), target_(target), screen_rec_(std::move(screen_rec)) {
    ring_.set_waker(&wake_);
    thread_ = std::thread(&DvrSink::run, this);
}

DvrSink::~DvrSink() {
    stop_.store(true);
    wake_.poke();
    thread_.join();
}

// ---- the radio thread ----------------------------------------------------------------

bool DvrSink::post(Item&& it) {
    if (ps_owed_) {                                // the parameter sets first, in order
        Item p;
        p.kind = Item::Params;
        p.ps = ps_owed_;
        if (!ring_.push(std::move(p))) return false;
        ps_owed_.reset();
    }
    return ring_.push(std::move(it));
}

void DvrSink::on_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) {
    ps_owed_ = std::make_shared<const std::vector<std::vector<uint8_t>>>(ps);   // the newest wins
    Item p;
    p.kind = Item::Params;
    p.ps = ps_owed_;
    if (ring_.push(std::move(p))) ps_owed_.reset();
}

// pic->pts is the picture's time: its capture stamp, else its arrival.
void DvrSink::on_picture(const bus::PictureRef& pic) {
    if (screen_rec_ && screen_rec_()) return;
    if (!target_.is_recording()) {
        if (rec_queued_) {                         // the end of one: the sink's thread winds it up
            Item it;
            it.kind = Item::Stopped;
            if (post(std::move(it))) rec_queued_ = false;
        }
        return;
    }
    rec_queued_ = true;
    Item it;
    it.pts = pic->pts;
    it.session = target_.session();
    it.whole = pic->whole;
    if (pic->pool->free_count() < kPoolReserve) {
        static uint64_t copied = 0;
        if (++copied <= 3 || copied % 1000 == 0)
            printf("DVR: behind - picture copied out of the radio's pool (%llu so far)\n",
                   (unsigned long long)copied);
        it.copy = std::make_shared<std::vector<uint8_t>>(pic->data(), pic->data() + pic->len);
    } else {
        it.pic = pic;
    }
    post(std::move(it));
}

// ---- the sink's thread -----------------------------------------------------------------

void DvrSink::run() {
    pthread_setname_np(pthread_self(), "dvr_in");
    Item it;
    uint64_t dropped = 0;
    while (!stop_.load()) {
        wake_.wait(500000);
        while (ring_.pop(it)) {
            take(it);
            it = Item();                           // the pool buffer goes back now, not at the next one
        }
        const uint64_t d = ring_.drops();
        if (d != dropped) {
            printf("DVR: %llu pictures came faster than the recording took them (%llu so far)\n",
                   (unsigned long long)(d - dropped), (unsigned long long)d);
            dropped = d;
        }
    }
}

void DvrSink::take(Item& it) {
    if (it.kind == Item::Params) {
        target_.set_parameter_sets(*it.ps);
        if (codec_ == VideoCodec::H265) repair_.set_parameter_sets(*it.ps);
        return;
    }
    if (it.kind == Item::Stopped) {
        if (rec_was_ && (repair_.pictures_filled() || repair_.slices_filled()))
            printf("DVR: the radio lost %u pictures and %u slices of that recording - copies put in their place\n",
                   repair_.pictures_filled(), repair_.slices_filled());
        rec_was_ = false;
        keyed_ = false;
        return;
    }
    RecordingTarget &rec = target_;
    if (!rec.is_recording() || it.session != rec.session()) return;
    if (session_ != it.session) {session_=it.session; rec_was_=false; keyed_=false;}
    if (!rec_was_) {
        rec_was_ = true;
        repair_.reset();
    }
    const uint8_t* data = it.copy ? it.copy->data() : it.pic->data();
    const size_t len = it.copy ? it.copy->size() : it.pic->len;
    // A recording of the camera's stream starts on a keyframe: the MP4 writer
    // takes no picture before one. With intra refresh the air unit sends
    // none unless asked, so a recording got none at all ("0 pictures
    // written"). While a recording has had no keyframe, one is asked for
    // every second, as the resync does.
    if (!keyed_) {
        if (has_keyframe(data, len)) {
            keyed_ = true;
        } else if (now_ms() - idr_asked_ms_ >= 1000) {
            idr_asked_ms_ = now_ms();
            printf("ar8030: recording without a keyframe yet - asking for one\n");
            keyframe_wanted_.store(true);   // the radio asks, after its next picture
        }
    }
    if (!keyed_) return;
    // A picture or slice the radio lost: a copy in its place, so the pictures
    // after it find what they refer to - Apple's decoder stopped at the first
    // loss and waited for a keyframe that never came (see dvr/hevc_repair).
    if (codec_ == VideoCodec::H265 && repair_.active()) {
        const std::vector<uint8_t> au(data, data + len);
        for (auto &u : repair_.process(au, it.pts, it.whole))
            rec.feed(std::make_shared<std::vector<uint8_t>>(std::move(u.au)), u.us);
        return;
    }
    if (it.copy) rec.feed(it.copy, it.pts);
    else rec.feed(it.pic, it.pts);
}

bool DvrSink::has_keyframe(const uint8_t* au, size_t n) const {
    for (size_t i = 0; i + 3 < n; i++) {
        if (au[i] != 0 || au[i + 1] != 0 || au[i + 2] != 1) continue;
        const uint8_t h = au[i + 3];
        if (codec_ == VideoCodec::H265) {
            const int t = (h >> 1) & 0x3F;
            if (t >= 16 && t <= 21) return true;
        } else if ((h & 0x1F) == 5) {
            return true;
        }
        i += 3;
    }
    return false;
}

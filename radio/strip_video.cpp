#include "radio/strip_video.hpp"
#include <cstdio>

namespace radio {

StripVideo::StripVideo(VideoCodec codec, bool decode_enabled, Model& model, VideoOut::Source& src, MakeDecoder make,
                       std::unique_ptr<hal::StripJoiner> joiner)
    : codec_(codec), decode_enabled_(decode_enabled), model_(model), src_(src), make_(std::move(make)),
      joiner_(std::move(joiner)) {}

StripVideo::~StripVideo() {
    stop_ = true;
    for (Strip& s : strip_)
        if (s.thread) pthread_join(s.thread, nullptr);
    for (Strip& s : strip_) {
        s.out.reset();
        s.dec.reset();
    }
}

// Strip k's VideoOut and decoder, made when its first slice comes. Whole
// pictures only (no stream decode): a picture of a strip is one slice, and a
// streamed one would hold the hardware decoder from the other strips.
bool StripVideo::make_strip(int k) {
    Strip& s = strip_[k];
    if (s.out) return true;
    static const char* const tags[kMax] = {"strip0", "strip1", "strip2", "strip3"};
    std::unique_ptr<Vdec> dec;
    if (decode_enabled_) {
        dec = make_([this, k](std::shared_ptr<DecodedUnit> du) { joiner_->strip(k, n_.load(), ov_.load(), std::move(du)); },
                    &stop_);
        if (!dec) return false;
    }
    s.dec = std::shared_ptr<Vdec>(std::move(dec));
    s.src.reset(new Src(*this, k));
    s.out.reset(new VideoOut(tags[k], codec_, s.dec, decode_enabled_, false, model_, *s.src));
    if (s.dec && pthread_create(&s.thread, nullptr, Vdec::run_frame_thread, s.dec.get())) {
        perror("strips: decoder thread");
        s.thread = 0;
        s.out.reset();
        s.dec.reset();
        return false;
    }
    printf("strips: strip %d decoded on its own%s\n", k, s.dec ? "" : " (decoding off)");
    return true;
}

bool StripVideo::nal(const uint8_t* nal, size_t len, uint64_t recv_us, VideoOut::SliceHeader& hdr) {
    const int k = hdr.idx;
    if (k < 0 || k >= kMax || !make_strip(k)) return false;
    if (hdr.last) n_ = k + 1;
    ov_ = hdr.strip_ov;
    // The picture's number on: the header has its low 16 bits.
    if (hdr.hseq >= 0) {
        if (pic_ < 0) pic_ = (uint16_t)hdr.hseq;
        else pic_ += (int16_t)((uint16_t)hdr.hseq - (uint16_t)pic_);
    }
    // To the strip's own stream a picture is one slice, which ends it.
    VideoOut::SliceHeader h = hdr;
    h.idx = 0;
    h.last = true;
    h.ends = true;
    h.pic = pic_;
    const bool taken = strip_[k].out->nal(nal, len, recv_us, h);
    // What the strip's VideoOut used up (with a slice), used up here too.
    hdr.hseq = h.hseq;
    hdr.out_us = h.out_us;
    if (h.idx < 0) { hdr.idx = -1; hdr.last = false; hdr.ends = false; }
    return taken;
}

void StripVideo::picture_start(int k, uint64_t recv_us, VideoOut::PictureTiming& t) {
    (void)k;
    // The first strip of a picture to arrive has its capture timed by the
    // radio; the rest have the same capture.
    if (timed_pic_ != pic_) {
        src_.picture_start(recv_us, t);
        timed_ = t;
        timed_pic_ = pic_;
        timed_recv_us_ = recv_us;
        return;
    }
    t = timed_;
    if (t.measured || t.air_us) t.air_us += (uint32_t)(recv_us - timed_recv_us_);
}

void StripVideo::picture_done(int k, const VideoOut::PictureDone& d) {
    // One picture for the radio's figures: from its top strip's first byte
    // to its bottom strip's last.
    if (k == 0) {
        done_ = d;
        done_pic_ = pic_;
    }
    if (k != n_.load() - 1) return;
    VideoOut::PictureDone p = d;
    if (done_pic_ == pic_) {
        p.measured = done_.measured;
        p.first_recv_us = done_.first_recv_us;
        p.air_us = done_.air_us;
        p.enc_us = done_.enc_us;
    }
    done_pic_ = -1;
    src_.picture_done(p);
}

void StripVideo::end_of_read() {
    for (Strip& s : strip_)
        if (s.out) s.out->end_of_read();
}

void StripVideo::drop_picture() {
    for (Strip& s : strip_)
        if (s.out) s.out->drop_picture();
    if (joiner_) joiner_->drop();
    timed_pic_ = done_pic_ = -1;
}

void StripVideo::relearn() {
    for (Strip& s : strip_)
        if (s.out) s.out->relearn();
}

}  // namespace radio

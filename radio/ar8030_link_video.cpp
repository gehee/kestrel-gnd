// The AR8030 link's video: the stream from the baseband (or a replay) split
// into slices for VideoOut, the pictures' timing and keyframes.
#include "radio/ar8030_link_detail.hpp"

using namespace radio;

void Ar8030Link::drain_video_socket() {
    if (sockfd < 0) return;
    std::vector<uint8_t> tmp(16384);
    for (int i = 0; i < 64; i++) {
        int n = bb_socket_read(sockfd, tmp.data(), (uint32_t)tmp.size(), 1);
        if (n <= 0) break;
    }
}

// A picture handed to the decoder whose capture was measured (VideoOut): its
// first and last slice's arrival, against the capture, and every 5 s the spread.
void Ar8030Link::picture_done(const radio::VideoOut::PictureDone& d) {
    if (!d.measured) return;
    const uint64_t cap = d.first_recv_us - d.air_us;
    ap_enc_.push_back(d.enc_us);
    ap_first_.push_back(d.air_us);
    if (d.last_recv_us >= cap) ap_all_.push_back((uint32_t)(d.last_recv_us - cap));
    const uint64_t t = now_ms();
    if (!ap_log_ms_) ap_log_ms_ = t;
    if (t - ap_log_ms_ < 5000) return;
    printf("ar8030: latency by the radio clock (%s), %zu pictures: capture->encoder out p50 %.2f "
           "p99 %.2f | capture->first slice here p50 %.2f p99 %.2f | capture->all slices here "
           "p50 %.2f p99 %.2f ms | per slice, air unit's radio->here p50 %.2f p99 %.2f ms\n",
           (air_feat_ & kAirFeatApClock) ? "us stamps" : "ms stamps", ap_first_.size(),
           pct_ms(ap_enc_, 50), pct_ms(ap_enc_, 99), pct_ms(ap_first_, 50), pct_ms(ap_first_, 99),
           pct_ms(ap_all_, 50), pct_ms(ap_all_, 99), pct_ms(ap_link_, 50), pct_ms(ap_link_, 99));
    printf("ar8030: per slice, air unit's radio->here by place: first %zu p50 %.2f p99 %.2f | "
           "middle %zu p50 %.2f p99 %.2f | last %zu p50 %.2f p99 %.2f ms | capture->last slice "
           "out of the encoder p50 %.2f p99 %.2f ms | sensor readout, learned, %.2f ms, the air unit's %.2f ms\n",
           ap_link_pos_[0].size(), pct_ms(ap_link_pos_[0], 50), pct_ms(ap_link_pos_[0], 99),
           ap_link_pos_[1].size(), pct_ms(ap_link_pos_[1], 50), pct_ms(ap_link_pos_[1], 99),
           ap_link_pos_[2].size(), pct_ms(ap_link_pos_[2], 50), pct_ms(ap_link_pos_[2], 99),
           pct_ms(ap_enc_last_, 50), pct_ms(ap_enc_last_, 99), video_.readout_us() / 1000.0, air_readout_us_ / 1000.0);
    ap_enc_.clear(); ap_first_.clear(); ap_all_.clear(); ap_link_.clear(); ap_enc_last_.clear();
    for (auto &v : ap_link_pos_) v.clear();
    ap_log_ms_ = t;
}

// A picture's first slice, into a buffer (VideoOut): its capture, measured by
// the radio's clock when there is a capture for it, the floor estimate
// otherwise; the air delay's figure with it.
void Ar8030Link::picture_start(uint64_t recv_us, radio::VideoOut::PictureTiming& t) {
    t.measured = pic_cap_us_ && recv_us > pic_cap_us_ && recv_us - pic_cap_us_ < 1000000;
    t.air_unknown = false;
    if (t.measured) {
        t.air_us = (uint32_t)(recv_us - pic_cap_us_);
        t.enc_us = std::min(std::min(pic_enc_us_, t.air_us), 0xffffu);
        const float ms = (float)t.air_us / 1000.0f;
        air_delay_ms = (air_delay_ms < 0.0f) ? ms : 0.95f * air_delay_ms + 0.05f * ms;
        model_.air_delay(air_delay_ms);
        air_known_ms_ = now_ms();
    } else if (pic_air_unknown_) {
        // The radio clock not checked yet: no air figure at all (the HUD shows
        // the ground's part only after a second of these).
        t.air_us = 0;
        t.enc_us = 0;
        t.air_unknown = true;
        if (now_ms() - air_known_ms_ > 1000) model_.clear_air_delay();
    } else {
        t.air_us = air_delay_for(recv_us);
        t.enc_us = 0;
    }
    // Its slices' own times, from its capture by the radio's clock - or, for
    // a --debug-replay capture from before the radio clock, its arrival, so
    // the slices still have rows (what a picture in halves is cut at) on the
    // bench.
    const bool bench = !t.measured && !replay_path.empty();
    t.frame_cap_us = t.measured ? pic_cap_us_ : bench ? recv_us : 0;
    t.height = hdr_height_;
    t.readout_us = air_readout_us_.load();
    t.exposure_us = air_exposure_us_.load();
    t.cap_us = cap_valid ? cap_us64 : 0;
    pic_cap_us_ = 0;   // one picture, one capture
    pic_air_unknown_ = false;
}

// Whole pictures or strips, as each header says: a change drops the picture
// the other way was building.
void Ar8030Link::vhdr_strip_seen(bool strip) {
    if (strip != vstrip_) {
        if (strip) video_.drop_picture();
        else if (strips_) strips_->whole_pictures();
        printf("ar8030: %s\n", strip ? "pictures in strips now: each strip decoded on its own, joined again"
                                     : "whole pictures again");
    }
    vstrip_ = strip;
}

bool Ar8030Link::ask_keyframe(const char* why) {
    if (now_ms() - idr_asked_ms_ < 1500) return false;
    idr_asked_ms_ = now_ms();
    if (why) printf("ar8030: %s\n", why);
    request_air_idr();
    return true;
}

// Emit one NAL (payload without the leading start code) to the decoder.
void Ar8030Link::emit_nal(const uint8_t* nal, size_t len) {
    if (len == 0) return;

    // The AR8030 multiplexes side-channel packets (RTP-like, first byte 0x80)
    // into the video stream, and an RF bit-error can corrupt a real NAL header.
    // Either way forbidden_zero_bit==1 means this is NOT a valid H.26x NAL.
    // Folding such bytes into a picture's access unit makes the rkvdec2 hardware
    // hang (task timeout -> reset storm -> blocky). Drop them so only clean
    // slices reach the decoder (ffmpeg and the stock firmware skip them too).
    if (nal[0] & 0x80) {
        nal_count++;
        // Not junk: the air unit's per-picture metadata header, repeated
        // ahead of each slice. Layout, from captures against known settings:
        //   [9]      fps        (0x64=100, 0x3C=60)
        //   [12..13] picture counter LE
        //   [14..15] width  LE  (0x0780=1920, 0x0500=1280)
        //   [16..17] height LE  (0x0438=1080, 0x02D0=720)
        //   [18..21] capture stamp LE u32: the air unit's CLOCK_MONOTONIC in
        //            microseconds, the same clock as its encoder PTS
        // Validate before trusting: the Annex-B splitter can hit a false start
        // code inside a header and hand us a shifted fragment. A good header
        // has a plausible resolution and a non-zero fps.
        if (len >= 22) {
            unsigned w   = nal[14] | (nal[15] << 8);
            unsigned h   = nal[16] | (nal[17] << 8);
            unsigned fps = nal[9];
            bool sane = (w >= 160 && w <= 4096) && (h >= 120 && h <= 2160) &&
                        (fps > 0 && fps <= 240);
            if (sane) {
                // Frame counter - detect pictures the link dropped.
                uint16_t hseq = (uint16_t)(nal[12] | (nal[13] << 8));
                const uint32_t cap32 = (uint32_t)nal[18] | ((uint32_t)nal[19] << 8) |
                                       ((uint32_t)nal[20] << 16) | ((uint32_t)nal[21] << 24);
                if (ltrace::on())
                    ltrace::rec(ltrace::kHdr, now_us(), hseq, cap32, nal, len);
                // The header repeats for each slice of a picture, so anything
                // measured per picture has to be gated on the counter changing.
                const bool new_pic = !hdr_seq_valid_ || hseq != hdr_seq_;
                hdr_seq_ = hseq;
                hdr_seq_valid_ = true;
                vhdr_.hseq = hseq;
                // [32]: the slice's number in the picture | 0x20 more follow |
                // 0x40 the last. Neither flag: not said.
                const bool numbered = len >= 33 && (nal[32] & 0x60);
                vhdr_.idx = numbered ? (nal[32] & 0x1f) : -1;
                vhdr_.last = numbered && (nal[32] & 0x40);
                // 0x80, kestrel-air's: the slice is a strip, its own stream.
                const bool strip = numbered && (nal[32] & 0x80) && len >= 42 && nal[33] == 0x4B;
                vhdr_strip_seen(strip);
                // [8], a strip's: the rows it codes past each of its joins.
                vhdr_.strip_ov = strip ? nal[8] : 0;
                // From kestrel-air, a picture can be decoded on it (video_out).
                vhdr_.ends = vhdr_.last && air_kestrel_;
                // A picture from the past, and everything up to the next
                // header with it: nothing it says about the link is true.
                if (new_pic) hdr_stale_ = stale_picture(cap32);
                if (hdr_stale_) return;
                // kestrel-air tags every header (byte 33 'K', 10 protocol, 11 features).
                if (len >= 42 && nal[33] == 0x4B) {
                    air_hdr_tag_ = true;
                    air_untagged_ = 0;
                    note_air_announce(true, nal[10], nal[11]);
                } else if (len >= 42 && air_hdr_tag_ && ++air_untagged_ >= 200) {
                    // The tag stopped (another air unit, or KA_LATINFO=0): the version
                    // message decides again. 200 headers, so one misread cannot clear it.
                    air_hdr_tag_ = false;
                    air_untagged_ = 0;
                    air_ver_seen_ = false;
                } else if (!air_hdr_tag_ && !air_ver_seen_ && ++air_stock_hdrs_ >= 200) {
                    // Video and never the tag: the stock air app, which sends its
                    // version message only in some states (none at all after a
                    // link-up on 2026-10-20). A kestrel-air without the tag says
                    // otherwise in its version message, which then decides.
                    air_stock_hdrs_ = 0;
                    note_air_announce(false, 0, 0);
                }
                if (air_kestrel_ && (air_feat_ & kAirFeatLatInfo) && len >= 42)
                    note_air_latency(nal);
                note_ap_delay(nal, len, new_pic);
                // The capture stamp, unwrapped: 32 bits of microseconds last
                // 71.6 minutes. A wrap is a step back whose way forward across
                // the top of the range took as long here as it says (an outage
                // in between too); any other step back is a new clock (the air
                // unit started again), not a wrap. Any step back by over half
                // the range was taken for one: an air unit restarted after its
                // 36th minute moved the recording's time on by up to 36 min.
                const uint64_t local_us = now_us();
                if (cap_valid && cap32 < cap_last32) {
                    const uint64_t across = (uint32_t)(cap32 - cap_last32);
                    const uint64_t here = local_us - cap_last_local_us;
                    if (across <= here + 2000000 + here / 50) cap_wraps++;
                }
                cap_last32 = cap32;
                cap_last_local_us = local_us;
                cap_us64 = ((uint64_t)cap_wraps << 32) | cap32;
                cap_valid = true;
                hdr_fps = fps;
                if (new_pic) {
                    if (hseq_valid) {
                        uint16_t gap = (uint16_t)(hseq - last_hseq);
                        if (gap > 1 && gap < 1000) {
                            frames_lost += (gap - 1);
                        }
                    }
                    last_hseq = hseq;
                    hseq_valid = true;
                }

                // Are we live yet? Compare how far apart two pictures were
                // captured with how far apart they arrived. Draining a backlog
                // those disagree wildly - 16.7ms of capture in under 4ms of
                // wall time; live they converge.
                if (resync_ && new_pic) {
                    uint32_t cap_hdr = (uint32_t)nal[18] | ((uint32_t)nal[19] << 8) |
                                       ((uint32_t)nal[20] << 16) | ((uint32_t)nal[21] << 24);
                    uint64_t arr_us = now_us();
                    if (resync_prev_us_) {
                        uint32_t d_cap = cap_hdr - resync_prev_cap_;
                        uint64_t d_arr = arr_us - resync_prev_us_;
                        // Skip the outage gap itself and any header we misread:
                        // only ordinary frame spacing says anything about pace.
                        if (d_cap > 0 && d_cap < 1000000) {
                            if (d_arr * 10 >= (uint64_t)d_cap * 6) resync_live_++;
                            else                                   resync_live_ = 0;
                        }
                    }
                    resync_prev_us_  = arr_us;
                    resync_prev_cap_ = cap_hdr;
                }
            }
        }
        return;
    }

    uint8_t nal_type = (codec == VideoCodec::H265) ? ((nal[0] >> 1) & 0x3F)
                                                   : (nal[0] & 0x1F);

    // Not every "NAL" the splitter finds is video. The air unit interleaves
    // small blocks of its own (26-49 bytes, e.g. "01 80 07 38 04 .." carrying
    // 1920x1080, or "0N 00 00 00 64 00" counting up) that follow a start-code
    // pattern, and whose first byte reads as a slice type - roughly one every
    // 36 pictures at 1080p120. Added to a picture, they were shrugged off by
    // MPP and FFmpeg but made the iPhone's decoder fail the web stream within
    // half a second. A header the spec forbids is not a NAL: forbidden bit
    // set, a layer other than the base layer, or temporal id 0.
    if (codec == VideoCodec::H265 && len >= 2 &&
        ((nal[0] & 0x80) || (((nal[0] & 1) << 5) | (nal[1] >> 3)) != 0 || (nal[1] & 7) == 0)) {
        static uint64_t foreign = 0;
        if (++foreign <= 3 || (foreign % 1000) == 0)
            printf("ar8030: skipped a non-video block in the video stream (%zu bytes, "
                   "hdr %02X %02X; %llu so far)\n", len, nal[0], nal[1],
                   (unsigned long long)foreign);
        return;
    }

    // The air unit's IMU samples (kestrel-air --imu) ride in SEI after each picture's last
    // slice: read out and published (radio/air_imu). Taken whatever the state of the
    // picture; the decoder drops the SEI itself below.
    if (codec == VideoCodec::H265 && (nal_type == 39 || nal_type == 40) && imu_out_.watched())
        imu_out_.sei(nal, len);

    // Everything up to the next header belongs to a stale picture's.
    if (hdr_stale_) return;

    bool is_vcl = (codec == VideoCodec::H265) ? (nal_type <= 31) : (nal_type >= 1 && nal_type <= 5);

    // Resync gate. After a reacquire, everything up to the first keyframe of the
    // new link is dropped - it is either the tail of the old stream or inter
    // frames with no reference to decode against.
    //
    // Parameter sets are the exception and pass through: the decoder caches them
    // and prepends them to the next IDR, so dropping them would leave the
    // keyframe we are waiting for undecodable.
    if (resync_) {
        const bool ps = (codec == VideoCodec::H265)
                            ? (nal_type == 32 || nal_type == 33 || nal_type == 34)
                            : (nal_type == 7 || nal_type == 8);
        // The same test the decoder uses for is_key (keyframe_nal), so what
        // opens the stream here is exactly what it will treat as a keyframe.
        const bool key = keyframe_nal(codec, nal_type);
        if (!ps) {
            const uint64_t held = now_ms() - resync_start_ms_;
            const bool timed_out = held > kResyncMaxMs;
            const bool caught_up = resync_live_ >= kResyncLive || timed_out;
            if (is_vcl && key && caught_up) {
                resync_ = false;
                printf("ar8030: resync - live after %llums (%s), video resumes\n",
                       (unsigned long long)held,
                       timed_out ? "timed out, showing it anyway" : "arriving at capture rate");
            } else {
                // Intra refresh has no keyframes to join on, and the ones the
                // air sent at link-up came before the video did: once the
                // stream is live, ask for one, again every 1.5 s - a keyframe
                // on a new link (5 MHz, low MCS) takes longer than half a
                // second to arrive, and each ask meant another one.
                if (is_vcl && !key && resync_live_ >= kResyncLive && air_kestrel_ &&
                    (air_feat_ & kAirFeatIntraRefresh) && now_ms() - idr_asked_ms_ >= 1500) {
                    idr_asked_ms_ = now_ms();
                    request_air_idr();
                }
                return;                       // still catching up
            }
        }
    }

    // Live video: into pictures, to the decoder and the sinks (radio/video_out).
    // When the bytes that completed this NAL came off USB: on ar_libre the
    // kernel's stamp for the read that brought them, free of how long this
    // thread took to be scheduled; otherwise now.
    vhdr_.fps = hdr_fps_;
    const uint64_t rx_us = read_rx_us ? read_rx_us : now_us();
    if (vstrip_) {
        // A strip: into its own stream (radio/strip_video).
        if (!strips_ && strips_supported())
            strips_.reset(new radio::StripVideo(codec, decode_enabled, model_, *this, strip_decoder_, strip_joiner_()));
        if (!strips_ || !strips_->nal(nal, len, rx_us, vhdr_)) return;
    } else if (!video_.nal(nal, len, rx_us, vhdr_)) {
        return;
    }

    update_stats(len + 4);

    nal_count++;
}

// Is the picture this header opens one from the past? Pictures are captured in
// order, so a live one is always newer than the newest seen - by a frame
// interval, or by the length of an outage. One that is not is a replay of the
// air unit's encoder ring (see live_cap_), or a new clock after the air unit
// restarted; the second keeps running forward, uninterrupted by any live
// picture, and after kStaleAdopt of those it is taken as the live stream.
bool Ar8030Link::stale_picture(uint32_t cap32) {
    // Differences in wrapping 32-bit microseconds: good for 35 minutes either way.
    if (!live_cap_valid_ || (int32_t)(cap32 - live_cap_) > 0) {
        live_cap_ = cap32;
        live_cap_valid_ = true;
        stale_run_ = 0;
        return false;
    }
    const int32_t step = (int32_t)(cap32 - stale_prev_cap_);
    stale_run_ = (stale_run_ > 0 && step > 0 && step < 1000000) ? stale_run_ + 1 : 1;
    stale_prev_cap_ = cap32;
    if (stale_run_ >= kStaleAdopt) {
        printf("ar8030: the air unit's capture clock went back %.1f s and stayed - "
               "a restart; following it\n", (double)(int32_t)(live_cap_ - cap32) / 1e6);
        live_cap_ = cap32;
        stale_run_ = 0;
        return false;
    }
    if (++stale_pics <= 3 || (stale_pics % 1000) == 0)
        printf("ar8030: dropped a picture captured %.3f s before the newest one "
               "(%llu so far)\n", (double)(int32_t)(live_cap_ - cap32) / 1e6, stale_pics);
    return true;
}








// --debug-replay: the video stream from a file in place of the baseband's
// socket - the same splitter, picture assembly, decoder, DVR and web feeds,
// with no air unit and no link. For finding out what a damaged stream does to
// them (tools/ar8030_fuzz.py makes one from any H.265 file). Read in the air
// unit's ~4K bursts at about 11 Mbit/s, round and round until stopped, with
// two seconds of nothing between passes: the air app stopping and starting
// its stream again, which is how a restart looks from here.
void Ar8030Link::run_replay() {
    FILE* f = fopen(replay_path.c_str(), "rb");
    if (!f) {
        printf("ar8030: replay: cannot open %s\n", replay_path.c_str());
        return;
    }
    printf("ar8030: replaying %s in place of the baseband\n", replay_path.c_str());
    std::vector<uint8_t> buf(4096);
    unsigned passes = 0;
    bool read_any = false;
    while (!*should_stop) {
        const size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            if (!read_any) break;              // empty file
            rewind(f);
            printf("ar8030: replay: pass %u done\n", ++passes);
            for (int i = 0; i < 20 && !*should_stop; i++) usleep(100000);
            continue;
        }
        read_any = true;
        total_bytes += n;
        last_data_ms = now_ms();
        model_.take_commands(*this);
        consume(buf.data(), n);
        usleep(3000);
    }
    fclose(f);
    printf("ar8030: replay stopped (%llu frames)\n", (unsigned long long)video_.frames_seen());
}

// Split an Annex-B byte stream into NALs. Handles NALs straddling reads by
// keeping the tail in `accum` until the next start code arrives.
//
// That alone holds every packet's last NAL until the next packet: the first
// slice of a picture waited for the second (~5 ms at 1080p100, the encoder's
// gap between them), and the last for whatever came next. The air unit's own
// framing says where a packet ends - its header (first byte 0x80) gives the
// slice's length (bytes 4..7, with any SEI after it) and the extras' (byte
// 31), and the packet closes with ee 29 55 9f - so once that trailer is in,
// the packet's last NAL goes on at once, with the same bytes the next start
// code would have given it. A header that does not add up, or a trailer that
// is not there, leaves it to the start code.
// One read of the radio's video: its NALs, then the slices held from it.
void Ar8030Link::consume(const uint8_t* data, size_t len) {
    consume_bytes(data, len);
    video_.end_of_read();
    if (strips_) strips_->end_of_read();
    check_rec_stream();
}

// A recording of the stream: strips are no one stream to record (each its
// own, of its rows), so while it runs the air unit is asked for whole
// pictures, and for its strips again once it stops. A SCREEN recording takes
// the joined pictures and needs none of this.
void Ar8030Link::check_rec_stream() {
    const bool rec = video_.sinks_need_stream();
    if (rec == rec_stream_) return;
    rec_stream_ = rec;
    if (air_strips_max_ < 2 || strips.load() < 2) return;
    printf("ar8030: %s\n", rec ? "a STREAM recording - whole pictures asked of the air unit while it runs"
                               : "the STREAM recording stopped - strips asked of the air unit again");
    strips_asked_ms_ = now_ms();
    request_setting(CAM_STRIPS, strips_wanted());
}




// The slice checksum of the packet whose header came last, once its payload
// is all in accum: XOR of its little-endian u32 words, then of the bytes left
// over (kestrel-air's ka_xor32). A mismatch is a slice the radio delivered
// damaged but whole - nothing else counts those: the decoder takes it and
// draws garbage blocks until intra refresh repairs them.
void Ar8030Link::check_slice_xor() {
    if (!xor_pending_) return;
    if (xor_start_ < accum_base_) { xor_pending_ = false; xor_unchecked_++; return; }
    const uint64_t end = xor_start_ + xor_len_;
    if (end > accum_base_ + accum.size()) return;          // not all here yet
    const uint8_t *b = accum.data() + (size_t)(xor_start_ - accum_base_);
    uint32_t x = 0;
    for (uint32_t i = 0; i + 4 <= xor_len_; i += 4)
        x ^= (uint32_t)b[i] | (uint32_t)b[i + 1] << 8 | (uint32_t)b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
    for (uint32_t i = xor_len_ & ~3u; i < xor_len_; i++) x ^= b[i];
    xor_pending_ = false;
    xor_checked_++;
    if (x != xor_want_) {
        xor_bad_++;
        if (xor_bad_ <= 5 || xor_bad_ % 100 == 0)
            printf("ar8030: a slice arrived damaged: checksum %08x, its packet says %08x (%u bytes; %llu of %llu)\n",
                   x, xor_want_, xor_len_, (unsigned long long)xor_bad_, (unsigned long long)xor_checked_);
    }
}

void Ar8030Link::consume_bytes(const uint8_t* data, size_t len) {
    accum.insert(accum.end(), data, data + len);

    if (accum.size() > AR_MAX_ACCUM) {
        printf("ar8030: no start code in %zu bytes - wrong port or non-AnnexB stream?\n",
               accum.size());
        accum_base_ += accum.size();
        pkt_end_pos_ = 0;
        accum.clear();
        return;
    }

    size_t i = 0, n = accum.size();
    size_t nal_start = SIZE_MAX;   // offset of current NAL payload

    while (i + 3 < n) {
        bool sc4 = (accum[i] == 0 && accum[i+1] == 0 && accum[i+2] == 0 && accum[i+3] == 1);
        bool sc3 = (accum[i] == 0 && accum[i+1] == 0 && accum[i+2] == 1);
        if (sc4 || sc3) {
            size_t sc_len = sc4 ? 4 : 3;
            if (nal_start != SIZE_MAX) {
                // NAL runs from nal_start up to this start code.
                emit_nal(accum.data() + nal_start, i - nal_start);
            }
            nal_start = i + sc_len;
            i += sc_len;
            if (nal_start >= n) break;         // the start code ends the buffer: its NAL comes next time
            if (accum[nal_start] != 0x80) continue;
            // A packet header is not Annex-B: its bytes can hold a start code.
            // The capture time's high bytes (18..25) are 01 00 00 00 or zeros,
            // so a radio clock (26..29) ending in 01 makes 00 00 00 01 - about
            // one header in 256. Cut there, the header's tail read as a header
            // of its own now and then (a mode of its own, a capture stamp from
            // nowhere: 16 pictures dropped as stale). So a header is skipped
            // whole, with its extras, once its checksum (byte 1, the sum of
            // bytes 2..41) says it is one; until it is all here, nothing past
            // its start is scanned.
            if (nal_start + 42 > n) break;
            const uint8_t *h = accum.data() + nal_start;
            uint8_t sum = 0;
            for (int k = 2; k < 42; k++) sum += h[k];
            const uint32_t ext = h[31];
            if (sum == h[1] && ext <= 64) {
                if (nal_start + 42 + ext > n) break;
                i = nal_start + 42 + ext;
                // The packet before it is all here by now: its checksum, then
                // this one's (06 02 <XOR32> in the extras), to check later.
                check_slice_xor();
                if (xor_pending_) { xor_pending_ = false; xor_unchecked_++; }
                const uint32_t pl = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
                                    ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
                for (uint32_t e = 0; e + 6 <= ext && pl > 0 && pl < AR_MAX_ACCUM;) {
                    const uint8_t *t = h + 42 + e;
                    if (t[0] < 2) break;
                    if (t[0] == 6 && t[1] == 0x02) {
                        xor_want_ = (uint32_t)t[2] | (uint32_t)t[3] << 8 | (uint32_t)t[4] << 16 | (uint32_t)t[5] << 24;
                        xor_start_ = accum_base_ + nal_start + 42 + ext;
                        xor_len_ = pl;
                        xor_pending_ = true;
                        break;
                    }
                    e += t[0];
                }
            }
            // A packet header, whole: where its packet ends.
            const uint32_t plen = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
                                  ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
            pkt_end_pos_ = (plen > 0 && plen < AR_MAX_ACCUM && ext <= 64)
                ? accum_base_ + nal_start + 42 + ext + plen + 4 : 0;
            continue;
        }
        i++;
    }

    // The open NAL is its packet's last and the packet is all here: on with it.
    if (nal_start != SIZE_MAX && pkt_end_pos_ > accum_base_ + nal_start &&
        pkt_end_pos_ <= accum_base_ + n) {
        const size_t pe = (size_t)(pkt_end_pos_ - accum_base_);
        if (accum[pe - 4] == 0xee && accum[pe - 3] == 0x29 && accum[pe - 2] == 0x55 &&
            accum[pe - 1] == 0x9f) {
            emit_nal(accum.data() + nal_start, pe - nal_start);
            check_slice_xor();
            accum.erase(accum.begin(), accum.begin() + pe);
            accum_base_ += pe;
            pkt_end_pos_ = 0;
            return;
        }
        pkt_end_pos_ = 0;
    }

    check_slice_xor();
    // Keep the unterminated tail (from the last start code we saw, or the last
    // 3 bytes which might be a partial start code).
    if (nal_start != SIZE_MAX) {
        size_t keep_from = nal_start - (nal_start >= 4 ? 4 : nal_start);
        accum.erase(accum.begin(), accum.begin() + keep_from);
        accum_base_ += keep_from;
    } else if (accum.size() > 3) {
        accum_base_ += accum.size() - 3;
        accum.erase(accum.begin(), accum.end() - 3);
    }
}

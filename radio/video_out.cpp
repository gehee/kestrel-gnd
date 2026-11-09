#include "radio/video_out.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include "base/frame_mode.hpp"
#include "base/prof.hpp"
#include "base/video_pool.hpp"
#include "radio/model.hpp"
#include "base/ltrace.hpp"

namespace radio {

static inline uint64_t now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

// ---- H.265 parameter-set sanity --------------------------------------------
//
// A two-byte header that looks right is not enough to trust a VPS/SPS/PPS: a
// 31-byte "SPS" with a valid header but garbage inside (it reads as SPS id 47,
// where 0-15 is the whole range) got cached next to the real one, went into
// every hvcC from then on, and the iPhone refused to decode the web stream at
// all. So the id is read and range-checked, and an SPS must also carry a
// chroma format and a picture size that exist.

namespace {
struct Rbsp {
    std::vector<uint8_t> b;
    size_t bit = 0;
    bool bad = false;
    Rbsp(const uint8_t* p, size_t n) {           // drop emulation-prevention bytes
        int zeros = 0;
        for (size_t i = 0; i < n; i++) {
            if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }
            b.push_back(p[i]);
            zeros = p[i] == 0 ? zeros + 1 : 0;
        }
    }
    uint32_t u(int n) {
        uint32_t v = 0;
        while (n--) {
            if (bit >= b.size() * 8) { bad = true; return 0; }
            v = (v << 1) | ((b[bit >> 3] >> (7 - (bit & 7))) & 1);
            bit++;
        }
        return v;
    }
    uint32_t ue() {
        int z = 0;
        while (!bad && u(1) == 0) if (++z > 31) { bad = true; return 0; }
        return z ? (1u << z) - 1 + u(z) : 0;
    }
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? (int32_t)((k >> 1) + 1) : -(int32_t)(k >> 1);
    }
    // rbsp_trailing_bits: a one, then nothing but zeros to the end.
    bool at_end() {
        if (bad || u(1) != 1) return false;
        while (bit < b.size() * 8) if (u(1)) return false;
        return true;
    }
};
}  // namespace

// The id of an H.265 VPS/SPS/PPS (payload after the 2-byte header), or -1 if
// the set cannot be real.
static int hevc_param_set_id(int type, const uint8_t* p, size_t n) {
    Rbsp r(p, n);
    if (type == 32) {                                   // VPS
        const uint32_t id = r.u(4);
        r.u(2);                                         // base layer internal/available
        const uint32_t layers = r.u(6), sub = r.u(3);
        r.u(1);                                         // temporal_id_nesting
        const uint32_t reserved = r.u(16);              // vps_reserved_0xffff_16bits
        return (r.bad || layers != 0 || sub > 6 || reserved != 0xFFFF) ? -1 : (int)id;
    }
    if (type == 34) {                                   // PPS
        const uint32_t id = r.ue();
        return (r.bad || id > 63) ? -1 : (int)id;
    }
    if (type != 33) return -1;                          // SPS
    r.u(4);                                             // sps_video_parameter_set_id
    const uint32_t max_sub = r.u(3);                    // sps_max_sub_layers_minus1
    r.u(1);                                             // temporal_id_nesting
    if (max_sub > 6) return -1;
    // profile_tier_level(1, max_sub)
    r.u(88); r.u(8);                                    // general profile + level
    uint32_t prof[8] = {0}, lvl[8] = {0};
    for (uint32_t i = 0; i < max_sub; i++) { prof[i] = r.u(1); lvl[i] = r.u(1); }
    if (max_sub > 0) for (uint32_t i = max_sub; i < 8; i++) r.u(2);
    for (uint32_t i = 0; i < max_sub; i++) { if (prof[i]) r.u(88); if (lvl[i]) r.u(8); }
    const uint32_t id = r.ue();
    const uint32_t chroma = r.ue();
    if (chroma == 3) r.u(1);                            // separate_colour_plane_flag
    const uint32_t w = r.ue(), h = r.ue();
    if (r.bad || id > 15 || chroma > 3 || w < 16 || h < 16 || w > 8192 || h > 8192) return -1;
    return (int)id;
}

// How many bits a slice_segment_address takes in pictures of this SPS -
// Ceil(Log2(PicSizeInCtbsY)) - or 0 if the SPS does not parse. The same walk as
// hevc_param_set_id, carried on to the coding block sizes.
static int hevc_sps_addr_bits(const uint8_t* p, size_t n, uint32_t* ctb_out = nullptr,
                              uint32_t* w_out = nullptr) {
    Rbsp r(p, n);
    r.u(4);
    const uint32_t max_sub = r.u(3);
    r.u(1);
    if (max_sub > 6) return 0;
    r.u(88); r.u(8);
    uint32_t prof[8] = {0}, lvl[8] = {0};
    for (uint32_t i = 0; i < max_sub; i++) { prof[i] = r.u(1); lvl[i] = r.u(1); }
    if (max_sub > 0) for (uint32_t i = max_sub; i < 8; i++) r.u(2);
    for (uint32_t i = 0; i < max_sub; i++) { if (prof[i]) r.u(88); if (lvl[i]) r.u(8); }
    r.ue();                                             // sps_seq_parameter_set_id
    if (r.ue() == 3) r.u(1);                            // chroma_format_idc
    const uint32_t w = r.ue(), h = r.ue();
    if (r.u(1)) { r.ue(); r.ue(); r.ue(); r.ue(); }     // conformance window
    r.ue(); r.ue();                                     // bit depths
    r.ue();                                             // log2_max_pic_order_cnt_lsb_minus4
    const uint32_t ordering_all = r.u(1);
    for (uint32_t i = ordering_all ? 0 : max_sub; i <= max_sub; i++) { r.ue(); r.ue(); r.ue(); }
    const uint32_t min_cb = r.ue() + 3, diff = r.ue();
    if (r.bad || w < 16 || h < 16 || w > 8192 || h > 8192 || min_cb + diff < 4 || min_cb + diff > 6) return 0;
    const uint32_t ctb = 1u << (min_cb + diff);
    const uint32_t ctbs = ((w + ctb - 1) / ctb) * ((h + ctb - 1) / ctb);
    if (ctb_out) *ctb_out = ctb;
    if (w_out) *w_out = w;
    int bits = 0;
    while ((1u << bits) < ctbs) bits++;
    return bits;
}

// scaling_list_data(), read and range-checked; false if it cannot be.
static bool hevc_scaling_list_ok(Rbsp& r) {
    for (int size = 0; size < 4; size++)
        for (int m = 0; m < 6; m += (size == 3) ? 3 : 1) {
            if (!r.u(1)) {                              // scaling_list_pred_mode_flag
                if (r.ue() > (uint32_t)(size == 3 ? m / 3 : m)) return false;
                continue;
            }
            const int coefs = size == 0 ? 16 : 64;
            if (size > 1) {
                const int32_t dc = r.se();              // scaling_list_dc_coef_minus8
                if (dc < -7 || dc > 247) return false;
            }
            for (int i = 0; i < coefs; i++) {
                const int32_t d = r.se();               // scaling_list_delta_coef
                if (d < -128 || d > 127 || r.bad) return false;
            }
        }
    return !r.bad;
}

// Whether an H.265 PPS (payload after the 2-byte header) reads right to its
// rbsp_trailing_bits with every value in range; its SPS's id in *sps_id.
// Random bytes behind a good header all but never do - and MPP re-parses a
// PPS over the one in use, so one that fails half way leaves that one broken:
// seen as a SIGSEGV in its set_sps() on the next slice, the half-parsed PPS
// naming an SPS that did not exist. Extensions are refused: they belong to
// profiles beyond Main 10, which this decoder does not do.
static bool hevc_pps_ok(const uint8_t* p, size_t n, int* sps_id) {
    Rbsp r(p, n);
    const uint32_t pps_id = r.ue(), sid = r.ue();
    r.u(1); r.u(1); r.u(3); r.u(1); r.u(1);   // dependent slices .. cabac_init_present_flag
    if (r.ue() > 14 || r.ue() > 14) return false;   // num_ref_idx_l0/l1_default_active_minus1
    const int32_t qp = r.se();                         // init_qp_minus26
    r.u(1); r.u(1);                                    // constrained_intra_pred, transform_skip
    if (r.u(1) && r.ue() > 3) return false;            // diff_cu_qp_delta_depth
    const int32_t cb = r.se(), cr = r.se();
    if (pps_id > 63 || sid > 15 || qp < -38 || qp > 25 || cb < -12 || cb > 12 ||
        cr < -12 || cr > 12)
        return false;
    r.u(1); r.u(1); r.u(1); r.u(1);                    // chroma qp offsets .. transquant_bypass
    const uint32_t tiles = r.u(1);
    r.u(1);                                            // entropy_coding_sync_enabled_flag
    if (tiles) {
        const uint32_t cols = r.ue(), rows = r.ue();
        if (cols > 19 || rows > 21) return false;
        if (!r.u(1)) {                                 // uniform_spacing_flag
            for (uint32_t i = 0; i < cols; i++) r.ue();
            for (uint32_t i = 0; i < rows; i++) r.ue();
        }
        r.u(1);                                        // loop_filter_across_tiles
    }
    r.u(1);                                            // loop_filter_across_slices
    if (r.u(1)) {                                      // deblocking_filter_control_present
        r.u(1);
        if (!r.u(1)) {
            const int32_t beta = r.se(), tc = r.se();
            if (beta < -6 || beta > 6 || tc < -6 || tc > 6) return false;
        }
    }
    if (r.u(1) && !hevc_scaling_list_ok(r)) return false;
    r.u(1);                                            // lists_modification_present
    if (r.ue() > 4) return false;                      // log2_parallel_merge_level_minus2
    r.u(1);                                            // slice_segment_header_extension
    if (r.u(1) && r.u(8) != 0) return false;           // pps_extension_present, and which
    *sps_id = (int)sid;
    return r.at_end();
}

// A PPS's dependent_slice_segments_enabled_flag, with its id in *id; -1 if it
// does not parse.
static int hevc_pps_dependent_slices(const uint8_t* p, size_t n, int* id) {
    Rbsp r(p, n);
    const uint32_t pps_id = r.ue();
    r.ue();                                             // pps_seq_parameter_set_id
    const uint32_t dep = r.u(1);
    if (r.bad || pps_id > 63) return -1;
    *id = (int)pps_id;
    return (int)dep;
}

// Where in the picture a slice starts, in coding tree blocks (0 for a
// picture's first slice), or -1 if it cannot be read. p is the slice NAL's
// payload after its 2-byte header.
static int hevc_slice_address(int type, const uint8_t* p, size_t n, int addr_bits,
                              const uint8_t dep_slices[64]) {
    if (addr_bits <= 0) return -1;
    Rbsp r(p, n < 32 ? n : 32);
    if (r.u(1)) return 0;                               // first_slice_segment_in_pic_flag
    if (type >= 16 && type <= 23) r.u(1);               // no_output_of_prior_pics_flag
    const uint32_t pps = r.ue();
    if (r.bad || pps > 63) return -1;
    if (dep_slices[pps]) r.u(1);                        // dependent_slice_segment_flag
    const uint32_t addr = r.u(addr_bits);
    return r.bad ? -1 : (int)addr;
}

VideoOut::VideoOut(const char* tag, VideoCodec codec, std::shared_ptr<Vdec> vdec, bool decode_enabled,
                   bool stream_decode, Model& model, Source& src)
    : tag_(tag), codec_(codec), vdec_(std::move(vdec)), decode_enabled_(decode_enabled),
      stream_decode_(stream_decode), model_(model), src_(src) {}

void VideoOut::relearn() {
    last_slice_addr_ = -1;
    last_addr_cand_ = -1; last_addr_streak_ = 0; last_count_cand_ = 0; last_slice_count_ = 0;
    cut_last_hseq_ = -1; cut_period_ = 0; cut_confirms_ = 0; pics_since_cut_ = 0;
    readout_win_.clear(); readout_us_ = 0;
}

// One NAL of video (the radio has checked it is one, and that it is live).
bool VideoOut::nal(const uint8_t* nal, size_t len, uint64_t recv_us, SliceHeader& hdr) {
    const uint8_t nal_type = (codec_ == VideoCodec::H265) ? ((nal[0] >> 1) & 0x3F) : (nal[0] & 0x1F);
    const bool is_vcl = (codec_ == VideoCodec::H265) ? (nal_type <= 31) : (nal_type >= 1 && nal_type <= 5);
    // feed_packet_to_decoder expects the Annex-B start code present, so rebuild it.
    // A slice gets its start code where it is written, in the picture (au_append).
    static thread_local std::vector<uint8_t> unit;
    unit.clear();
    if (!is_vcl) {
        unit.reserve(len + 4);
        unit.insert(unit.end(), {0x00, 0x00, 0x00, 0x01});
        unit.insert(unit.end(), nal, nal + len);
    }

    // No slice before the decoder has a picture parameter set: it refuses
    // every one ("PPS id out of range"), and with intra refresh the sets only
    // come with a keyframe asked for. The resync opens on a keyframe, but its
    // parameter sets can be lost while the keyframe gets through (a new link
    // at 5 MHz, then re-formed at the step to 20) - and once past the resync
    // nothing asked again: video arrived and nothing was ever shown. Until a
    // valid PPS has come, slices are held back and a keyframe is asked for
    // every 1.5 s, as the resync does.
    if (codec_ == VideoCodec::H265 && is_vcl && !pps_ok_) {
        src_.ask_keyframe("slices but no picture parameter set yet - asking for a keyframe");
        return false;
    }
    // The sets in hand are not the decoder's: it takes them in front of a
    // keyframe, and only that. The resync can open on a keyframe's second
    // slice (its first came while the stream was still catching up), which
    // goes nowhere, and then no keyframe ever reached the decoder - every
    // slice after refused "PPS id out of range", the link up and no picture
    // until kestrel-gnd restarted (2026-10-22, 864p120). Until the decoder
    // has one, only keyframes pass, and one is asked for every 1.5 s.
    if (codec_ == VideoCodec::H265 && is_vcl && vdec_ && decode_enabled_ && !vdec_->keyframe_in() &&
        !hevc_irap(nal_type)) {
        src_.ask_keyframe("the decoder has no keyframe to decode against - asking for one");
        return false;
    }

    const bool is_param_set = (codec_ == VideoCodec::H265)
                                  ? (nal_type == 32 || nal_type == 33 || nal_type == 34)
                                  : (nal_type == 7 || nal_type == 8);

    // Validate a parameter set before anything takes it - the decoder too.
    // An RF bit-error can flip a slice NAL into something that *looks* like a
    // parameter set: we saw "41 1F ..." and "44 1F ..." land in hvcC, which
    // decode to nuh_layer_id 35 (the base layer must be 0) and made players
    // report "PPS id out of range" and refuse the file. MPP re-parses a PPS
    // over the one in use, so a bad one handed to it breaks every picture
    // after. Type alone is not enough - the rest of the header has to be sane.
    bool ps_valid = false;
    int ps_id = 0;
    if (is_param_set && len >= 6) {
        if (codec_ == VideoCodec::H265) {
            // 2-byte header: nuh_layer_id must be 0 and nuh_temporal_id_plus1 1,
            // which for a base-layer parameter set means byte 1 is exactly 0x01
            // and the low bit of byte 0 (the top layer_id bit) is clear. Then
            // the contents: an id in range, and for an SPS a real picture.
            ps_valid = ((nal[0] & 0x01) == 0) && (nal[1] == 0x01);
            if (ps_valid) {
                ps_id = hevc_param_set_id(nal_type, nal + 2, len - 2);
                ps_valid = ps_id >= 0;
            }
            // An SPS has to read on to its coding block sizes, and a PPS to
            // its end, naming an SPS that did.
            if (ps_valid && nal_type == 33) {
                ps_valid = hevc_sps_addr_bits(nal + 2, len - 2) > 0;
                if (ps_valid) sps_ids_ok_ |= (uint16_t)(1u << ps_id);
            } else if (ps_valid && nal_type == 34) {
                int sid = -1;
                ps_valid = hevc_pps_ok(nal + 2, len - 2, &sid) && (sps_ids_ok_ >> sid & 1);
                if (ps_valid) pps_ok_ = true;
            }
        } else {
            ps_valid = ((nal[0] & 0x60) != 0);   // nal_ref_idc != 0
        }
    }

    // The air unit sends each picture as two slice NALs, its two halves. Feeding each slice to
    // MPP as its own packet/PTS corrupts frame + reference assembly (partial
    // "top-only" pictures and an rkvdec timeout/reset storm). Accumulate all the
    // slices of one picture and feed them as a single access unit with one PTS.
    // Built for the video sinks that want whole pictures too, with or
    // without a decoder; everything said to the decoder stays behind decoding.
    const bool decoding = vdec_ && decode_enabled_;
    if (decoding || picture_sinks_) {
        if (is_vcl) {
            // Which picture the air unit says this slice is (-1: no header),
            // and which of its slices.
            const int hseq = hdr.hseq;
            const int idx = hdr.idx;
            const bool idx_last = hdr.last;
            hdr.hseq = -1;
            hdr.idx = -1;
            hdr.last = false;
            // first_slice_segment_in_pic_flag is the first bit after the HEVC
            // 2-byte NAL header; a 1 marks the start of a new picture.
            uint8_t first_slice = (codec_ == VideoCodec::H265)
                ? ((len >= 3) ? ((nal[2] >> 7) & 1) : 1)
                : 1;  // H.264 (unused for AR8030): one slice == one picture
            const int addr = (codec_ == VideoCodec::H265 && len >= 3)
                ? hevc_slice_address(nal_type, nal + 2, len - 2, ctb_addr_bits_, pps_dep_slices_)
                : -1;
            if (ltrace::on())
                ltrace::rec(ltrace::kSlice, recv_us, (uint32_t)addr,
                            (uint64_t)len | ((uint64_t)first_slice << 32) |
                            ((uint64_t)nal_type << 40));
            if (!first_slice && au_open_ &&
                (nal_type != au_nal_type_ || (addr >= 0 && addr <= au_max_addr_) ||
                 (hseq >= 0 && au_hseq_ >= 0 && hseq != au_hseq_))) {
                // Not this picture's: another picture's slice (one whose own
                // first slice was lost, or a stale one without a readable
                // header), or one of this picture's again. Appended, MPP
                // decodes two pictures as one ("POC change between slices")
                // and the hardware times out on the result.
                if (++foreign_slices_ <= 3 || (foreign_slices_ % 1000) == 0)
                    printf("%s: dropped a slice that is not the picture's being built "
                           "(type %d/%d, CTB %d after %d, picture %d/%d; %llu so far)\n", tag_,
                           nal_type, au_nal_type_, addr, au_max_addr_, hseq, au_hseq_,
                           foreign_slices_);
                return false;
            }
            if (first_slice && au_open_) {
                // Completed by the next picture's arrival: the fallback, and
                // how the last slice's start address gets learned.
                learn_last_slice(au_max_addr_, au_slice_count_);
                flush_access_unit(recv_us);
            }
            if (!first_slice && !au_open_) {
                // A slice for a picture already handed to the decoder early.
                // Mostly the air unit's stray fragments (28 bytes, reserved
                // types - they used to be appended to the next picture); a
                // run of them means the learned end was wrong, so relearn.
                late_slice();
                return false;
            }
            // A buffer for the new picture; none (the consumers hold them
            // all) and the picture is lost, as if the radio had lost it.
            if (first_slice && !au_begin()) return false;
            if (first_slice) {
                // Its capture, as the radio measured it.
                PictureTiming pt;
                src_.picture_start(recv_us, pt);
                au_measured_ = pt.measured;
                au_air_unknown_ = pt.air_unknown;
                au_air_us_ = pt.air_us;
                au_enc_us_ = pt.enc_us;
                // Its slices, each timed on its own - only with the capture
                // timed and the slices' rows known.
                au_times_.reset();
                if (pt.frame_cap_us && ctb_size_ && ctbs_per_row_ && pt.height) {
                    au_times_ = std::make_shared<SliceTimes>();
                    au_times_->height = pt.height;
                    au_times_->frame_cap_us = pt.frame_cap_us;
                    au_times_->readout_us = pt.readout_us;
                    au_times_->exposure_us = pt.exposure_us;
                    au_times_->key = keyframe_nal(codec_, nal_type);
                    au_times_->pts = (int64_t)frame_pts_;
                }
                // The air unit's capture stamp of this picture: its clock, not
                // ours - only the differences are used (the recording's sample
                // durations).
                au_cap_us_ = pt.cap_us;
                au_pts_ = frame_pts_;
                au_nal_type_ = nal_type;   // picture key-ness (IDR vs trailing)
                au_pic_->keyframe = keyframe_nal(codec_, nal_type);
                au_first_recv_us_ = recv_us;  // zero point for this picture
                au_max_addr_ = 0;
                au_hseq_ = hseq;
                au_slice_count_ = 0;
                au_idx_seen_ = 0;
                au_idx_last_ = -1;
                au_idx_unsaid_ = false;
                au_streamed_ = false;
                au_stream_done_ = false;
                frames_seen_++;            // count real pictures, not slices
                if (++late_window_pics_ >= 64) { late_window_pics_ = 0; late_slices_ = 0; }
            }
            uint32_t at = 0;
            if (!au_append(nal, len, &at)) {
                drop_au();
                return false;
            }
            const uint8_t* slice = au_pic_->data() + at;
            const uint32_t slice_len = (uint32_t)len + 4;
            note_slice(addr, first_slice, recv_us, hdr);
            au_last_recv_us_ = recv_us;   // newest slice of this picture
            au_open_ = true;
            if (addr > au_max_addr_) au_max_addr_ = addr;
            au_slice_count_++;
            if (idx < 0 || idx > 31) {
                au_idx_unsaid_ = true;
            } else {
                au_idx_seen_ |= 1u << idx;
                if (idx_last) au_idx_last_ = idx;
            }
            const bool last = last_slice_addr_ > 0 && addr == last_slice_addr_;
            // Streamed: the decoder starts on the first slice while the rest is
            // still on the air - the air unit sends the second half about half
            // a frame after the first - and finishes about a millisecond after
            // the last lands rather than a whole decode later.
            // touch /tmp/kestrel-stream-off: whole pictures again, without a
            // restart (an air unit often stops sending video when kestrel restarts).
            if (first_slice && (pause_check_++ % 50) == 0) {
                const bool off = access("/tmp/kestrel-stream-off", F_OK) == 0 ||
                                 g_frame_mode.load(std::memory_order_relaxed) == kFrameWholeDecode;
                if (off != stream_paused_)
                    printf("%s: stream decode %s (frame mode %s, /tmp/kestrel-stream-off)\n", tag_,
                           off ? "paused" : "resumed", frame_mode_label(g_frame_mode.load()));
                stream_paused_ = off;
            }
            if (first_slice) {
                // Keyframes go whole: they carry the parameter sets the decoder
                // needs to start on (see keyframe_in), and with intra refresh
                // come only when asked for - not worth a half-built start.
                if (decoding && stream_decode_ && !stream_paused_ && Vdec::stream_holds().load() == 0 &&
                    !last && last_slice_addr_ > 0 && !keyframe_nal(codec_, nal_type) &&
                    last_slice_count_ > 1 && vdec_->stream_supported()) {
                    if (!stream_pics_++)
                        printf("%s: decoding each picture from its first slice "
                               "(%d slices a picture, stream mode)\n", tag_, last_slice_count_);
                    feed_au(slice, slice_len, last_slice_count_);
                    au_streamed_ = true;
                }
            } else if (au_streamed_ && !au_stream_done_) {
                // Held to the end of this read (send_held_slices); the last
                // slice goes at once, with any held before it.
                if (held_pts_ != au_pts_ || held_end_ == held_off_) { held_off_ = at; held_pts_ = au_pts_; }
                held_end_ = at + slice_len;
                if (last) send_held_slices(true);
            }
            // The picture's last slice: decode it now, not when the next
            // picture's first slice turns up - a frame interval later (10 ms
            // at 100 fps), which is how long every complete picture used to
            // wait.
            if (last_slice_addr_ > 0 && addr == last_slice_addr_)
                flush_access_unit(recv_us);
        } else if (decoding && (!is_param_set || ps_valid)) {
            // VPS/SPS/PPS are cached and SEI dropped inside the decoder; the
            // cached param sets get prepended to the next IDR automatically.
            vdec_->feed_packet_to_decoder(unit.data(), (int)unit.size(), frame_pts_,
                                         recv_us, nal_type, 0, 0);
        }
    }

    // Keep our own copy of the parameter sets for the DVR. The decoder's cache
    // is private to it, and a recording started mid-stream needs VPS/SPS/PPS
    // ahead of its first IDR or the muxer emits an empty hvcC and the file
    // plays black. H.265: VPS 32, SPS 33, PPS 34. H.264: SPS 7, PPS 8.
    // Only valid sets (ps_valid, above), and exactly one of each type is
    // kept, so hvcC can never accumulate copies.
    if (is_param_set) {
        if (ps_valid) ps_since_pic_ = true;
        if (ps_valid) {
            // One set per (type, id). This stream uses more than one PPS, so a
            // single set per type left slices unable to resolve theirs
            // ("PPS changed between slices"); keyed by id, every PPS is kept,
            // and a newer set with an id already held - a camera mode change
            // sends a new SPS under the same id - replaces the old one rather
            // than sitting next to it in hvcC.
            auto key_of = [&](const std::vector<uint8_t>& q) {
                // q is Annex-B: 4-byte start code, 2-byte header, payload.
                const int t = (codec_ == VideoCodec::H265) ? ((q[4] >> 1) & 0x3F) : (q[4] & 0x1F);
                const int id = (codec_ == VideoCodec::H265)
                                   ? hevc_param_set_id(t, q.data() + 6, q.size() - 6) : 0;
                return std::make_pair(t, id);
            };
            const auto key = std::make_pair((int)nal_type, ps_id);
            bool known = false, changed = false;
            for (auto& q : param_sets_) {
                const bool same = q.size() == unit.size() &&
                                  memcmp(q.data(), unit.data(), unit.size()) == 0;
                if (same) { known = true; break; }
                if (codec_ == VideoCodec::H265 && key_of(q) == key) {
                    q = unit; known = changed = true; break;
                }
            }
            if (!known) {
                if (param_sets_.size() >= 16) param_sets_.erase(param_sets_.begin());
                param_sets_.push_back(unit);
                changed = true;
            }
            // What reading a slice's start address takes (hevc_slice_address).
            if (codec_ == VideoCodec::H265 && nal_type == 33) {
                uint32_t ctb = 0, w = 0;
                const int bits = hevc_sps_addr_bits(nal + 2, len - 2, &ctb, &w);
                if (bits > 0 && ctb) {
                    ctb_size_ = ctb;
                    ctbs_per_row_ = (w + ctb - 1) / ctb;
                }
                if (bits != ctb_addr_bits_) {
                    printf("%s: slice addresses are %d bits in this picture size\n", tag_, bits);
                    ctb_addr_bits_ = bits;
                    last_slice_addr_ = -1;              // a new picture size: learn again
                    last_addr_cand_ = -1; last_addr_streak_ = 0; last_count_cand_ = 0; last_slice_count_ = 0;
                }
            } else if (codec_ == VideoCodec::H265 && nal_type == 34) {
                int id = -1;
                const int dep = hevc_pps_dependent_slices(nal + 2, len - 2, &id);
                if (dep >= 0) pps_dep_slices_[id] = (uint8_t)dep;
            }
            if (changed) {
                for (bus::VideoSink* k : sinks_) k->on_parameter_sets(param_sets_);
                printf("%s: cached param set type=%d id=%d len=%zu (%zu total)\n", tag_,
                       nal_type, ps_id, unit.size(), param_sets_.size());
            }
        } else {
            ps_dropped_++;
            if (ps_dropped_ <= 5 || (ps_dropped_ % 100) == 0)
                printf("%s: dropped corrupt param-set NAL type=%d hdr=%02X %02X "
                       "(total %llu)\n", tag_, nal_type, nal[0], len > 1 ? nal[1] : 0,
                       (unsigned long long)ps_dropped_);
        }
    }

    // Parameter sets are deliberately NOT forwarded: the recorder replays the
    // cached ones once when recording starts. The air unit re-sends VPS/SPS/PPS
    // periodically, and feeding minimp4 the same set twice makes it append a
    // second, malformed entry to the hvcC arrays while leaving numNalus stale -
    // ffmpeg then rejects the file with "Invalid NAL unit size in extradata"
    // and QuickTime refuses it outright.
    // VCL NALs are accumulated and handed over as one access unit in
    // flush_access_unit(). Parameter sets are replayed by DvrRecorder at record
    // start. Anything else (SEI and friends) is left out: handed over on its
    // own it became an MP4 sample of its own - an empty "frame" with a frame's
    // duration between every two pictures - and nothing in it is needed to
    // play the file.
    return true;
}

// Where a picture's last slice starts, learned from pictures completed by the
// next one's arrival: the same start address as the last slice of kLearnRun
// pictures in a row. The air unit cuts every picture the same way - two
// halves, the second at CTB 255 of 510 at 1080p, IDR or not - so this settles
// within a second of video; a new picture size (SPS) starts it over.
void VideoOut::learn_last_slice(int max_addr, int slices) {
    if (last_slice_addr_ > 0 || max_addr <= 0) return;
    // Only a larger end starts over: a picture that ends early (a slice lost or
    // unreadable) says nothing about where pictures end. With intra refresh a
    // GOP start every 17 pictures did that, the run never reached kLearnRun, and
    // every picture waited for the next one to be decoded - a frame of latency
    // (2026-10-05). The slice count is learned with it (streamed pictures need
    // it): the same end with more slices starts over too, with fewer it is a
    // picture that lost one.
    if (max_addr == last_addr_cand_ && slices == last_count_cand_) last_addr_streak_++;
    else if (max_addr > last_addr_cand_ || (max_addr == last_addr_cand_ && slices > last_count_cand_)) {
        last_addr_cand_ = max_addr; last_count_cand_ = slices; last_addr_streak_ = 1;
    } else {
        static unsigned shorter = 0;
        if (++shorter <= 5 || shorter % 1000 == 0)
            printf("%s: a picture ended at CTB %d with %d slices, short of %d with %d - "
                   "not counted (%u so far)\n", tag_, max_addr, slices, last_addr_cand_,
                   last_count_cand_, shorter);
        return;
    }
    if (last_addr_streak_ >= kLearnRun) {
        last_slice_addr_ = max_addr;
        last_slice_count_ = slices;
        printf("%s: pictures end with the slice at CTB %d - decoding each as that "
               "slice arrives\n", tag_, last_slice_addr_);
    }
}

// A slice after its picture was flushed early. The air unit's stray fragments
// do this about once in 600 pictures; more than a few in 64 pictures means
// pictures do not end where learned (a mode change), so stop flushing early
// and learn again.
void VideoOut::late_slice() {
    late_slices_total_++;
    if (++late_slices_ > kLateMax && last_slice_addr_ > 0) {
        printf("%s: %u slices after their picture ended in %u pictures - relearning "
               "where pictures end\n", tag_, late_slices_, late_window_pics_);
        last_slice_addr_ = -1;
        last_addr_cand_ = -1; last_addr_streak_ = 0; last_count_cand_ = 0; last_slice_count_ = 0;
        late_slices_ = 0;
    }
}

// Hand the decoder a picture, or with stream_slices > 1 the first slice of one
// that many slices long (the rest by Vdec::stream_append), stamped as the
// picture open in au_*.
void VideoOut::feed_au(const uint8_t *data, size_t len, int stream_slices) {
    // This AU's OWN first-slice arrival, not the arrival of the read that
    // happens to be flushing it - see au_first_recv_us.
    const uint64_t au_recv = au_first_recv_us_ ? au_first_recv_us_ : now_us();
    if (ltrace::on()) ltrace::rec(ltrace::kAu, now_us(), (uint32_t)au_pts_, au_recv);
    if (ltrace::on() && au_measured_ && au_air_us_ && au_recv > au_air_us_)
        ltrace::rec(ltrace::kCap, au_recv, (uint32_t)au_pts_, au_recv - au_air_us_);
    // The air delay, capture stamp -> first slice here, in two parts.
    // Measured by the radio's clock: capture -> the encoder handing the
    // first slice out (the HUD's Enc), and from there to here (RF). By
    // the floor estimate (see air_delay_for) it is all in the second.
    // No exposure wait is added: an LED switched on in front of the
    // camera is in the frame whose capture stamp comes 0 ms later on the
    // median (-5..+5 ms, photodiode rig with the air unit and goggle
    // clocks synced), so the stamp already stands for the moment of
    // capture. The screen's own delay is not counted: the figure ends at
    // the vblank the picture goes out on.
    const uint32_t enc_us = au_air_us_ ? au_enc_us_ : 0;
    const uint32_t proc_us = au_air_unknown_ ? kAirUnknown : au_air_us_ - au_enc_us_;
    vdec_->set_next_slice_times(au_times_);
    if (stream_slices > 1)
        vdec_->stream_start((void *)data, (int)len, au_pts_, au_recv, au_nal_type_,
                           enc_us, proc_us, stream_slices);
    else
        vdec_->feed_packet_to_decoder((void *)data, (int)len, au_pts_, au_recv, au_nal_type_,
                                     enc_us, proc_us);
}

// Drop the access unit being built. One already streamed to the decoder is
// ended there - not left waiting for the kernel's deadline with the decoder
// held.
void VideoOut::drop_au() {
    if (au_streamed_ && !au_stream_done_ && vdec_) vdec_->stream_end(au_pts_);
    au_streamed_ = au_stream_done_ = false;
    au_pic_.reset();
    // The next picture gets a pts of its own, as after a flush: with this
    // one's, MPP refuses its streamed parts as late and the renderer takes
    // it for the picture dropped here.
    if (au_open_) frame_pts_++;
    au_open_ = false;
}

// A slice of the open access unit, for its own latency (base/slice_times.hpp):
// its first row, when that row was captured, when the encoder handed the slice
// out (its header, just before it) and when it got here. The capture of a row
// is the picture's capture stamp (row 0) plus the time the sensor takes to
// read down to it, which is learned here: a slice leaves the encoder a fixed
// tail after its last row is read, so the first and last slices' encoder-out
// times are apart by the readout of the rows between their ends.
void VideoOut::note_slice(int addr, bool first_slice, uint64_t recv_us, SliceHeader& hdr) {
    const uint64_t out = hdr.out_us;
    hdr.out_us = 0;
    const bool gop_start = first_slice && ps_since_pic_;   // parameter sets came just before it
    if (first_slice) ps_since_pic_ = false;
    SliceTimes *st = au_times_.get();
    if (!st || addr < 0 || (!first_slice && addr == 0)) return;
    const int i = st->n.load();
    if (i >= SliceTimes::kMax) return;
    const uint32_t row = (uint32_t)(addr / (int)ctbs_per_row_) * ctb_size_;
    if (row >= st->height) return;
    if (!readout_us_ && hdr.fps) readout_us_ = 900000u / hdr.fps;   // until learned: 90% of a frame
    if (first_slice) {
        st->expected = last_slice_count_;
        uint32_t end = predict_first_end(au_hseq_);
        // Parameter sets just before it: a GOP start, where kestrel-air's
        // intra refresh restarts and cuts the first slice short. Taking the
        // shorter end is safe either way (fewer rows shown early).
        if (gop_start && cut_row_ && end && cut_row_ < end) end = cut_row_;
        st->split_row.store(end);
    }
    if (i == 1 && ctbs_per_row_ && last_slice_addr_ > 0) {
        // The first slice's end, as it turned out: learn the cut pictures.
        const uint32_t normal = (uint32_t)(last_slice_addr_ / (int)ctbs_per_row_) * ctb_size_;
        pics_since_cut_++;
        if (row != normal && au_hseq_ >= 0) {
            if (cut_last_hseq_ >= 0) {
                const int d = (au_hseq_ - cut_last_hseq_ + 65536) % 65536;
                if (d == cut_period_) cut_confirms_++;
                else { cut_period_ = d; cut_confirms_ = 0; }
            }
            cut_last_hseq_ = au_hseq_;
            cut_row_ = row;
            pics_since_cut_ = 0;
        }
    }
    SliceTimes::Slice &s = st->s[i];
    s.row = row;
    const uint32_t ro = st->readout_us ? st->readout_us : readout_us_;   // the air unit's, else learned
    s.cap_us = st->frame_cap_us + (uint64_t)ro * row / st->height;
    s.out_us = out;
    s.here_us = recv_us;
    st->n.store(i + 1);
    const bool last = last_slice_addr_ > 0 && addr == last_slice_addr_;
    if (last) st->complete.store(true);
    if (last && i >= 1 && st->s[0].out_us && out > st->s[0].out_us) {
        const uint32_t span = st->height - st->s[1].row;      // last end - first end, in rows
        if (span > st->height / 8) {
            const uint64_t r = (out - st->s[0].out_us) * st->height / span;
            if (r >= 2000 && r <= 40000) {
                readout_win_.push_back((uint32_t)r);
                if (readout_win_.size() >= 64) {
                    std::vector<uint32_t> v = readout_win_;
                    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                    readout_us_ = v[v.size() / 2];
                    readout_win_.clear();
                }
            }
        }
    }
}

// Where picture hseq's first slice should end, row; 0 if that cannot be said
// with confidence (then it is not shown in halves before its second slice).
uint32_t VideoOut::predict_first_end(int hseq) const {
    if (!ctbs_per_row_ || last_slice_addr_ <= 0 || hseq < 0) return 0;
    // Two slices only (1080p above 60 fps): with four (60 fps) the first one
    // ends a quarter down and that is not learned yet.
    if (last_slice_count_ != 2) return 0;
    const uint32_t normal = (uint32_t)(last_slice_addr_ / (int)ctbs_per_row_) * ctb_size_;
    // No cut for a long while (none at all yet, or the refresh is off now):
    // every picture is cut at the usual place - after enough of them to say so.
    if (cut_last_hseq_ < 0) return pics_since_cut_ > 100 ? normal : 0;
    if (pics_since_cut_ > 400) return normal;
    if (cut_period_ <= 0 || cut_confirms_ < 1) return 0;     // cuts seen, their rhythm not yet
    const int d = (hseq - cut_last_hseq_ + 65536) % 65536;
    return d % cut_period_ == 0 ? cut_row_ : normal;
}

// Feed the accumulated slices of one picture as a single access unit / PTS.
// A streamed one is in the decoder already: if its last slice never came, it
// is told there is no more.
void VideoOut::flush_access_unit(uint64_t recv_us) {
    if (!au_open_) return;
    if (au_times_) au_times_->complete.store(true);   // no more slices for it
    const bool have = au_pic_ && au_pic_->len;
    if (have && vdec_ && decode_enabled_) {
        const uint64_t au_recv = au_first_recv_us_ ? au_first_recv_us_ : recv_us;
        if (!au_streamed_) {
            feed_au(au_pic_->data(), au_pic_->len, 0);
        } else if (!au_stream_done_) {
            send_held_slices();
            vdec_->stream_end(au_pts_);
            if (++stream_ended_ <= 3 || (stream_ended_ % 1000) == 0)
                printf("%s: picture %lld ended without its last slice "
                       "(%llu of %llu streamed pictures)\n", tag_, (long long)au_pts_,
                       (unsigned long long)stream_ended_, (unsigned long long)stream_pics_);
        }
        au_streamed_ = au_stream_done_ = false;
        PictureDone d;
        d.measured = au_measured_;
        d.first_recv_us = au_first_recv_us_;
        d.last_recv_us = au_last_recv_us_;
        d.air_us = au_air_us_;
        d.enc_us = au_enc_us_;
        src_.picture_done(d);
        frame_pts_++;

        // Reassembly span: first slice of THIS picture on the wire to its
        // last. Must use au_last_recv_us, not the flush-triggering read -
        // that read is the NEXT picture's first slice, so it would measure
        // the inter-frame interval (~10ms at 100fps) instead.
        //
        // Expect 0.0 much of the time: recv_us is stamped per socket read,
        // and a whole picture usually arrives inside one read, so all its
        // slices share a timestamp. Non-zero means the picture straddled
        // reads - which is exactly the case worth seeing, since that is when
        // waiting for a complete picture actually costs latency.
        if (prof::enabled() && au_last_recv_us_ >= au_recv) {
            // KESTREL_PROF: what the reassembly stage is made of - the span from a picture's
            // first slice to its last, against the picture's size, for key and other pictures.
            static std::vector<float> span[2];
            static double bytes[2] = {0, 0};
            static uint64_t last_print = 0;
            const bool key = keyframe_nal(codec_, au_nal_type_);
            span[key ? 1 : 0].push_back((float)(au_last_recv_us_ - au_recv) / 1000.0f);
            bytes[key ? 1 : 0] += (double)au_pic_->len;
            const uint64_t tnow = now_us();
            if (tnow - last_print > 2000000) {
                if (last_print) {
                    for (int kk = 0; kk < 2; kk++) {
                        auto& s = span[kk];
                        if (s.empty()) continue;
                        std::sort(s.begin(), s.end());
                        int zero = 0;
                        for (float x : s) if (x < 0.05f) zero++;
                        printf("PROF AU %s: n=%zu span ms p50 %.1f p90 %.1f max %.1f, same-read %d, avg %.1f kB\n",
                               kk ? "key  " : "other", s.size(), s[s.size() / 2], s[s.size() * 9 / 10], s.back(), zero,
                               bytes[kk] / (double)s.size() / 1000.0);
                    }
                    fflush(stdout);
                }
                last_print = tnow;
                span[0].clear(); span[1].clear(); bytes[0] = bytes[1] = 0;
            }
        }
        if (model_.viewers() && au_last_recv_us_ >= au_recv) {
            float rsm_ms = (float)(au_last_recv_us_ - au_recv) / 1000.0f;
            if (rsm_ms < 200.0f) {   // ignore absurd outliers
                rsm_ewma_ = (rsm_ewma_ < 0.0f) ? rsm_ms : 0.9f * rsm_ewma_ + 0.1f * rsm_ms;
                model_.reassembly(rsm_ewma_);
            }
        }
    }
    // The DVR gets the same complete access unit. Feeding it slice by slice
    // made minimp4 emit one MP4 sample per slice, so a player saw a "frame"
    // holding a single slice and dropped the rest with "PPS changed between
    // slices" - decoding only the top quarter of the picture and painting the
    // remainder green. One picture must be one sample.
    //
    // Its time: the capture stamp for the sample's duration; arrival times
    // only from an air unit that sends no stamps (then every one is).
    //
    // Whole: every number up to the one said to be the last came. What the
    // DVR's repair needs - a picture of an unusual shape (the intra refresh's
    // sweep starts come as 2 slices, not 4) is not one that lost slices.
    if (have) {
        au_pic_->pts = au_cap_us_ ? au_cap_us_ : au_first_recv_us_;
        au_pic_->whole = au_idx_unsaid_ ? -1
                       : au_idx_last_ >= 0 && au_idx_seen_ == (uint32_t)((2ull << au_idx_last_) - 1) ? 1 : 0;
        for (bus::VideoSink* k : sinks_) k->on_picture(au_pic_);
        // A sink that needs a keyframe (a recording starts on one) has it
        // asked of the air unit.
        for (bus::VideoSink* k : sinks_) if (k->take_keyframe_request()) src_.request_keyframe();
    }
    au_pic_.reset();
    au_open_ = false;
}

// Two appends of the same picture within microseconds (its middle slices in
// one read, the last one later) made the RK3568 end the picture before its
// last slice now and then (11 of 78 such pictures at 1080p60): that slice was
// refused and the picture's lower part stayed wrong until the intra refresh
// passed. So the slices of one read go in one append - as a whole picture
// goes, all its slices in one part - and the last slice takes the held ones
// with it.
void VideoOut::send_held_slices(bool last) {
    if (held_end_ == held_off_ && !last) return;
    if (held_pts_ == au_pts_ && au_streamed_ && !au_stream_done_ && vdec_ && au_pic_) {
        if (!vdec_->stream_append(au_pic_->data() + held_off_, (int)(held_end_ - held_off_), au_pts_, last) &&
            (++stream_refused_ <= 3 || (stream_refused_ % 1000) == 0))
            printf("%s: the decoder did not take a slice of picture %lld "
                   "(%llu so far)\n", tag_, (long long)au_pts_,
                   (unsigned long long)stream_refused_);
        au_stream_done_ = last;
    }
    held_off_ = held_end_ = 0;
    held_pts_ = -1;
}

// A buffer of the video pool for the picture about to be built.
bool VideoOut::au_begin() {
    au_pic_ = video_pool::get().acquire();
    if (au_pic_) {
        held_off_ = held_end_ = 0;
        held_pts_ = -1;
        return true;
    }
    if (++au_no_buffer_ <= 3 || (au_no_buffer_ % 1000) == 0)
        printf("%s: no buffer free for a picture - dropped (%llu so far)\n", tag_,
               (unsigned long long)au_no_buffer_);
    return false;
}

// One slice into the open picture, with its start code; at: where it starts.
bool VideoOut::au_append(const uint8_t* nal, size_t len, uint32_t* at) {
    bus::PictureBuf* b = au_pic_.get();
    if (!b) return false;
    if (b->len + len + 4 > b->cap) {
        if (++au_no_buffer_ <= 3 || (au_no_buffer_ % 1000) == 0)
            printf("%s: a picture larger than %u bytes - dropped (%llu so far)\n", tag_,
                   b->cap, (unsigned long long)au_no_buffer_);
        return false;
    }
    uint8_t* d = b->data() + b->len;
    d[0] = d[1] = d[2] = 0; d[3] = 1;
    memcpy(d + 4, nal, len);
    *at = b->len;
    b->len += (uint32_t)len + 4;
    return true;
}

}  // namespace radio

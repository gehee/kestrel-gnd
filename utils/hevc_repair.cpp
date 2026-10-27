// A recording of the air unit's H.265 stream repaired where the radio lost
// part of it: copy pictures and copy slices. See hevc_repair.hpp.
#include "utils/hevc_repair.hpp"

#include <algorithm>
#include <cstdio>

namespace {

constexpr int kMaxFill = 500;   // a longer gap is a dropout, not a loss: left as it is

// ---- bits ------------------------------------------------------------------

struct BitReader {
    const uint8_t* p;
    size_t n, pos = 0;   // in bits
    bool bad = false;
    BitReader(const uint8_t* d, size_t len) : p(d), n(len * 8) {}
    uint32_t u(int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits; i++) {
            if (pos >= n) { bad = true; return 0; }
            v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (!u(1)) { if (bad || ++zeros > 31) { bad = true; return 0; } }
        return zeros ? ((1u << zeros) - 1 + u(zeros)) : 0;
    }
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }
};

struct BitWriter {
    std::vector<uint8_t> b;
    int bits = 0;   // used in the last byte
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if (bits == 0) b.push_back(0);
            if ((v >> i) & 1) b.back() |= (uint8_t)(0x80 >> bits);
            bits = (bits + 1) & 7;
        }
    }
    void ue(uint32_t v) {
        const uint32_t x = v + 1;
        int len = 0;
        while ((x >> len) > 1) len++;
        put(0, len);
        put(x, len + 1);
    }
    void se(int32_t v) { ue(v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v)); }
    void align_one_zeros() {   // byte_alignment(): a one, then zeros
        put(1, 1);
        while (bits) put(0, 1);
    }
};

int ceil_log2(int v) {
    int n = 0;
    while ((1 << n) < v) n++;
    return n;
}

// The NAL's RBSP (emulation prevention bytes taken out), at most max bytes.
std::vector<uint8_t> rbsp_of(const uint8_t* d, size_t len, size_t max = (size_t)-1) {
    std::vector<uint8_t> r;
    r.reserve(std::min(len, max));
    int zeros = 0;
    for (size_t i = 0; i < len && r.size() < max; i++) {
        if (zeros >= 2 && d[i] == 3) { zeros = 0; continue; }
        r.push_back(d[i]);
        zeros = d[i] ? 0 : zeros + 1;
    }
    return r;
}

// An RBSP as a NAL with its start code, emulation prevention put in.
std::vector<uint8_t> nal_of(int nal_type, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> o = {0, 0, 0, 1, (uint8_t)(nal_type << 1), 1};
    int zeros = 0;
    for (uint8_t c : rbsp) {
        if (zeros >= 2 && c <= 3) { o.push_back(3); zeros = 0; }
        o.push_back(c);
        zeros = c ? 0 : zeros + 1;
    }
    return o;
}

// NALs of an Annex-B buffer: offset of the start code, offset of the header, end.
struct NalPos { size_t sc, hdr, end; };
std::vector<NalPos> split_nals(const std::vector<uint8_t>& a) {
    std::vector<NalPos> v;
    const size_t n = a.size();
    for (size_t i = 0; i + 3 <= n; i++) {
        if (a[i] == 0 && a[i + 1] == 0 && a[i + 2] == 1) {
            const size_t sc = (i > 0 && a[i - 1] == 0) ? i - 1 : i;
            if (!v.empty()) v.back().end = sc;
            v.push_back({sc, i + 3, n});
            i += 2;
        }
    }
    return v;
}

// ---- CABAC, the encoder side (H.265 9.3.4.3 / 9.3.5) -------------------------

const uint8_t kLps[64][4] = {
    {128,176,208,240},{128,167,197,227},{128,158,187,216},{123,150,178,205},{116,142,169,195},
    {111,135,160,185},{105,128,152,175},{100,122,144,166},{95,116,137,158},{90,110,130,150},
    {85,104,123,142},{81,99,117,135},{77,94,111,128},{73,89,105,122},{69,85,100,116},
    {66,80,95,110},{62,76,90,104},{59,72,86,99},{56,69,81,94},{53,65,77,89},{51,62,73,85},
    {48,59,69,80},{46,56,66,76},{43,53,63,72},{41,50,59,69},{39,48,56,65},{37,45,54,62},
    {35,43,51,59},{33,41,48,56},{32,39,46,53},{30,37,43,50},{29,35,41,48},{27,33,39,45},
    {26,31,37,43},{24,30,35,41},{23,28,33,39},{22,27,32,37},{21,26,30,35},{20,24,29,33},
    {19,23,27,31},{18,22,26,30},{17,21,25,28},{16,20,23,27},{15,19,22,25},{14,18,21,24},
    {14,17,20,23},{13,16,19,22},{12,15,18,21},{12,14,17,20},{11,14,16,19},{11,13,15,18},
    {10,12,15,17},{10,12,14,16},{9,11,13,15},{9,11,12,14},{8,10,12,14},{8,9,11,13},
    {7,9,11,12},{7,9,10,12},{7,8,10,11},{6,8,9,11},{6,7,9,10},{6,7,8,9},{2,2,2,2}};
const uint8_t kTransLps[64] = {
    0,0,1,2,2,4,4,5,6,7,8,9,9,11,11,12,13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,33,33,34,34,35,35,35,36,36,36,37,37,
    37,38,38,63};

struct Ctx { uint8_t state, mps; };

Ctx ctx_init(int init_value, int qp) {
    const int m = (init_value >> 4) * 5 - 45, n = ((init_value & 15) << 3) - 16;
    int pre = ((m * std::min(std::max(qp, 0), 51)) >> 4) + n;
    pre = std::min(std::max(pre, 1), 126);
    return pre <= 63 ? Ctx{(uint8_t)(63 - pre), 0} : Ctx{(uint8_t)(pre - 64), 1};
}

struct Cabac {
    BitWriter& w;
    uint32_t low = 0, range = 510;
    int outstanding = 0;
    bool first = true;
    explicit Cabac(BitWriter& bw) : w(bw) {}
    void put_bit(int b) {
        if (first) first = false;
        else w.put(b, 1);
        for (; outstanding > 0; outstanding--) w.put(1 - b, 1);
    }
    void renorm() {
        while (range < 256) {
            if (low < 256) put_bit(0);
            else if (low >= 512) { low -= 512; put_bit(1); }
            else { low -= 256; outstanding++; }
            range <<= 1;
            low <<= 1;
        }
    }
    void decision(Ctx& c, int bin) {
        const uint32_t lps = kLps[c.state][(range >> 6) & 3];
        range -= lps;
        if (bin != c.mps) {
            low += range;
            range = lps;
            if (c.state == 0) c.mps = 1 - c.mps;
            c.state = kTransLps[c.state];
        } else if (c.state < 62) {
            c.state++;
        }
        renorm();
    }
    void terminate(int bin) {   // the last one (bin 1) also writes rbsp_stop_one_bit
        range -= 2;
        if (bin) {
            low += range;
            range = 2;
            renorm();
            put_bit((low >> 9) & 1);
            w.put(((low >> 7) & 3) | 1, 2);
        } else {
            renorm();
        }
    }
};

}  // namespace

// ---- parameter sets --------------------------------------------------------

void HevcRepair::set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) {
    bool have_sps = false, have_pps = false, supported = true;
    for (const auto& u : ps) {
        size_t i = 0;
        while (i + 3 <= u.size() && !(u[i] == 0 && u[i + 1] == 0 && u[i + 2] == 1)) i++;
        const uint8_t* nal = (i + 3 <= u.size()) ? u.data() + i + 3 : u.data();
        const size_t len = u.data() + u.size() - nal;
        if (len < 3) continue;
        const int type = (nal[0] >> 1) & 0x3f;
        const std::vector<uint8_t> r = rbsp_of(nal + 2, len - 2);
        BitReader b(r.data(), r.size());
        if (type == 33) {
            b.u(4);
            const int max_sub = b.u(3);
            b.u(1);
            // profile_tier_level(1, max_sub)
            b.u(88); b.u(8);
            std::vector<int> pp(max_sub), lp(max_sub);
            for (int k = 0; k < max_sub; k++) { pp[k] = b.u(1); lp[k] = b.u(1); }
            if (max_sub > 0) for (int k = max_sub; k < 8; k++) b.u(2);
            for (int k = 0; k < max_sub; k++) { if (pp[k]) b.u(88); if (lp[k]) b.u(8); }
            b.ue();                                    // sps id
            const int chroma = b.ue();
            if (chroma == 3 && b.u(1)) supported = false;   // separate colour planes
            chroma_ = chroma != 0;
            width_ = b.ue(); height_ = b.ue();
            if (b.u(1)) { b.ue(); b.ue(); b.ue(); b.ue(); }   // conformance window
            b.ue(); b.ue();                            // bit depths
            log2_poc_lsb_ = b.ue() + 4;
            const int ordering_all = b.u(1);
            for (int k = ordering_all ? 0 : max_sub; k <= max_sub; k++) { b.ue(); b.ue(); b.ue(); }
            log2_min_cb_ = b.ue() + 3;
            log2_ctb_ = log2_min_cb_ + b.ue();
            b.ue(); b.ue(); b.ue(); b.ue();            // transform sizes, depths
            if (b.u(1) && b.u(1)) supported = false;   // scaling list data in the SPS
            b.u(1);                                    // amp
            sao_ = b.u(1);
            if (b.u(1)) { b.u(4); b.u(4); b.ue(); b.ue(); b.u(1); }   // pcm
            num_st_rps_ = b.ue();
            sps_rps_neg_.clear();
            for (int k = 0; k < num_st_rps_; k++) {
                if (k != 0 && b.u(1)) { supported = false; break; }   // predicted sets
                const int neg = b.ue(), pos = b.ue();
                std::vector<int> d;
                int poc = 0;
                for (int j = 0; j < neg; j++) {
                    poc -= (int)b.ue() + 1;
                    if (b.u(1)) d.push_back(poc);
                }
                for (int j = 0; j < pos; j++) { b.ue(); b.u(1); }
                sps_rps_neg_.push_back(d);
            }
            lt_present_ = b.u(1);
            num_lt_sps_ = 0;
            if (lt_present_) {
                num_lt_sps_ = b.ue();
                for (int k = 0; k < num_lt_sps_; k++) { b.u(log2_poc_lsb_); b.u(1); }
            }
            sps_tmvp_ = b.u(1);
            have_sps = !b.bad;
        } else if (type == 34) {
            pps_id_ = b.ue();
            b.ue();
            if (b.u(1)) supported = false;             // dependent slices
            output_flag_ = b.u(1);
            num_extra_bits_ = b.u(3);
            b.u(1);                                    // sign data hiding
            cabac_init_present_ = b.u(1);
            num_ref_l0_default_ = b.ue() + 1;
            b.ue();
            init_qp_ = 26 + b.se();
            b.u(1); b.u(1);                            // constrained intra, transform skip
            if (b.u(1)) b.ue();                        // cu qp delta
            b.se(); b.se();
            chroma_qp_offsets_ = b.u(1);
            if (b.u(1)) supported = false;             // weighted prediction
            b.u(1);
            transquant_bypass_ = b.u(1);
            if (b.u(1)) supported = false;             // tiles
            if (b.u(1)) supported = false;             // wavefronts
            lf_across_slices_ = b.u(1);
            deblock_override_ = false;
            pps_deblock_disabled_ = false;
            if (b.u(1)) {
                deblock_override_ = b.u(1);
                pps_deblock_disabled_ = b.u(1);
                if (!pps_deblock_disabled_) { b.se(); b.se(); }
            }
            if (b.u(1)) supported = false;             // scaling list data in the PPS
            if (b.u(1)) supported = false;             // reference list modification
            b.ue();                                    // parallel merge level
            slice_ext_ = b.u(1);
            have_pps = !b.bad;
        }
    }
    const bool was = ok_;
    ok_ = have_sps && have_pps && supported && log2_ctb_ >= 4 && width_ > 0 && height_ > 0;
    if (ok_) {
        ctbs_w_ = (width_ + (1 << log2_ctb_) - 1) >> log2_ctb_;
        ctbs_h_ = (height_ + (1 << log2_ctb_) - 1) >> log2_ctb_;
        ctbs_ = ctbs_w_ * ctbs_h_;
        addr_bits_ = ceil_log2(ctbs_);
        lt_lsb_bits_ = num_lt_sps_ > 1 ? ceil_log2(num_lt_sps_) : 0;
    }
    if (ok_ != was || !ok_)
        printf("DVR: lost-picture repair %s (%dx%d, CTB %d)\n",
               ok_ ? "on" : "off - the stream uses what it does not handle",
               width_, height_, 1 << log2_ctb_);
    starts_.clear();
}

void HevcRepair::reset() {
    have_prev_ = false;
    pics_filled_ = slices_filled_ = 0;
}

// ---- slices ----------------------------------------------------------------

bool HevcRepair::parse_slice(const uint8_t* nal, size_t len, Slice& s) const {
    if (len < 4) return false;
    s.nal_type = (nal[0] >> 1) & 0x3f;
    s.rbsp = rbsp_of(nal + 2, len - 2, 256);
    BitReader b(s.rbsp.data(), s.rbsp.size());
    const bool first = b.u(1);
    if (s.nal_type >= 16 && s.nal_type <= 23) b.u(1);
    b.ue();
    s.addr = first ? 0 : (int)b.u(addr_bits_);
    b.u(num_extra_bits_);
    s.slice_type = b.ue();
    if (output_flag_) b.u(1);
    s.poc_lsb = 0;
    s.min_ref_delta = 0;
    s.tmvp = false;
    if (s.nal_type != 19 && s.nal_type != 20) {
        s.poc_lsb = b.u(log2_poc_lsb_);
        s.rps_bit0 = (int)b.pos;
        if (b.u(1)) {   // a set from the SPS
            const int idx = num_st_rps_ > 1 ? (int)b.u(ceil_log2(num_st_rps_)) : 0;
            if (idx < (int)sps_rps_neg_.size() && !sps_rps_neg_[idx].empty())
                s.min_ref_delta = sps_rps_neg_[idx][0];
        } else {
            if (num_st_rps_ != 0 && b.u(1)) return false;   // predicted from the SPS's
            const int neg = b.ue(), pos = b.ue();
            int poc = 0;
            for (int j = 0; j < neg; j++) {
                poc -= (int)b.ue() + 1;
                if (b.u(1) && !s.min_ref_delta) s.min_ref_delta = poc;
            }
            for (int j = 0; j < pos; j++) { b.ue(); b.u(1); }
        }
        if (lt_present_) {
            const int n_sps = num_lt_sps_ > 0 ? (int)b.ue() : 0;
            const int n = n_sps + (int)b.ue();
            for (int j = 0; j < n; j++) {
                if (j < n_sps) b.u(lt_lsb_bits_);
                else { b.u(log2_poc_lsb_); b.u(1); }
                if (b.u(1)) b.ue();
            }
        }
        s.rps_bit1 = (int)b.pos;
        if (sps_tmvp_) s.tmvp = b.u(1);
    }
    return !b.bad;
}

// A copy slice over CTBs [addr, end): P, every coding unit a skip. A whole
// picture (rps_from null) refers to the picture just before it, without
// temporal candidates, so each skip is a zero-motion copy of it. Part of a
// picture takes the picture's own reference set and temporal flag (both must
// be the same in all its slices).
std::vector<uint8_t> HevcRepair::copy_slice(int nal_type, int poc_lsb, int addr, int end,
                                            const Slice* rps_from) const {
    BitWriter w;
    w.put(addr == 0, 1);                       // first_slice_segment_in_pic_flag
    w.ue(pps_id_);
    if (addr) w.put(addr, addr_bits_);
    w.put(0, num_extra_bits_);
    w.ue(1);                                   // slice_type P
    if (output_flag_) w.put(1, 1);
    w.put(poc_lsb, log2_poc_lsb_);
    bool tmvp = false;
    if (rps_from) {
        BitReader b(rps_from->rbsp.data(), rps_from->rbsp.size());
        b.pos = rps_from->rps_bit0;
        for (int i = rps_from->rps_bit0; i < rps_from->rps_bit1; i++) w.put(b.u(1), 1);
        tmvp = rps_from->tmvp;
    } else {
        w.put(0, 1);                           // short_term_ref_pic_set_sps_flag
        if (num_st_rps_ != 0) w.put(0, 1);     // inter_ref_pic_set_prediction_flag
        w.ue(1); w.ue(0);                      // one negative picture, no positive
        w.ue(0); w.put(1, 1);                  // delta -1, used
        if (lt_present_) {
            if (num_lt_sps_ > 0) w.ue(0);
            w.ue(0);
        }
    }
    if (sps_tmvp_) w.put(tmvp, 1);
    if (sao_) { w.put(0, 1); if (chroma_) w.put(0, 1); }
    w.put(0, 1);                               // num_ref_idx_active_override_flag
    if (cabac_init_present_) w.put(0, 1);
    if (tmvp && num_ref_l0_default_ > 1) w.ue(0);   // collocated_ref_idx
    w.ue(4);                                   // five_minus_max_num_merge_cand: one candidate
    w.se(0);                                   // slice_qp_delta
    if (chroma_qp_offsets_) { w.se(0); w.se(0); }
    if (deblock_override_) w.put(0, 1);
    if (lf_across_slices_ && !pps_deblock_disabled_) w.put(1, 1);
    if (slice_ext_) w.ue(0);
    w.align_one_zeros();

    // slice_segment_data
    const int qp = init_qp_;
    Ctx split[3] = {ctx_init(107, qp), ctx_init(139, qp), ctx_init(126, qp)};
    Ctx skip[3] = {ctx_init(197, qp), ctx_init(185, qp), ctx_init(201, qp)};
    Ctx bypass = ctx_init(154, qp);
    Cabac c(w);
    const int min_w = (width_ + (1 << log2_min_cb_) - 1) >> log2_min_cb_;
    const int min_h = (height_ + (1 << log2_min_cb_) - 1) >> log2_min_cb_;
    std::vector<int8_t> depth(min_w * min_h, -1);   // -1: not ours (or not coded yet)
    auto avail = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
        const int ctb = (y >> log2_ctb_) * ctbs_w_ + (x >> log2_ctb_);
        return ctb >= addr && depth[(y >> log2_min_cb_) * min_w + (x >> log2_min_cb_)] >= 0;
    };
    auto quadtree = [&](auto&& self, int x0, int y0, int log2, int d) -> void {
        const int size = 1 << log2;
        bool sp;
        if (x0 + size <= width_ && y0 + size <= height_ && log2 > log2_min_cb_) {
            int inc = 0;
            if (avail(x0 - 1, y0) && depth[(y0 >> log2_min_cb_) * min_w + ((x0 - 1) >> log2_min_cb_)] > d) inc++;
            if (avail(x0, y0 - 1) && depth[((y0 - 1) >> log2_min_cb_) * min_w + (x0 >> log2_min_cb_)] > d) inc++;
            c.decision(split[inc], 0);
            sp = false;
        } else {
            sp = log2 > log2_min_cb_;
        }
        if (sp) {
            const int h = size / 2;
            for (int k = 0; k < 4; k++) {
                const int x = x0 + (k & 1) * h, y = y0 + (k >> 1) * h;
                if (x < width_ && y < height_) self(self, x, y, log2 - 1, d + 1);
            }
            return;
        }
        if (transquant_bypass_) c.decision(bypass, 0);
        const int inc = (avail(x0 - 1, y0) ? 1 : 0) + (avail(x0, y0 - 1) ? 1 : 0);
        c.decision(skip[inc], 1);              // cu_skip_flag; one merge candidate: no merge_idx
        for (int y = y0; y < std::min(y0 + size, height_); y += 1 << log2_min_cb_)
            for (int x = x0; x < std::min(x0 + size, width_); x += 1 << log2_min_cb_)
                depth[(y >> log2_min_cb_) * min_w + (x >> log2_min_cb_)] = (int8_t)d;
    };
    for (int a = addr; a < end; a++) {
        quadtree(quadtree, (a % ctbs_w_) << log2_ctb_, (a / ctbs_w_) << log2_ctb_, log2_ctb_, 0);
        c.terminate(a == end - 1);             // end_of_slice_segment_flag (+ the stop bit)
    }
    while (w.bits) w.put(0, 1);                // rbsp_alignment_zero_bit
    return nal_of(nal_type, w.b);
}

// ---- access units ----------------------------------------------------------

std::vector<HevcRepair::Unit> HevcRepair::process(const std::vector<uint8_t>& au, uint64_t us) {
    std::vector<Unit> out;
    auto pass = [&]() { out.push_back({au, us}); return out; };
    if (!ok_) return pass();

    const std::vector<NalPos> nals = split_nals(au);
    std::vector<Slice> slices;
    for (const NalPos& p : nals) {
        if (p.end <= p.hdr + 2) continue;
        const int type = (au[p.hdr] >> 1) & 0x3f;
        if (type > 31) continue;
        Slice s;
        if (!parse_slice(au.data() + p.hdr, p.end - p.hdr, s)) return pass();
        s.off = p.sc;
        s.len = p.end - p.sc;
        slices.push_back(std::move(s));
    }
    if (slices.empty()) return pass();
    const Slice& s0 = slices[0];
    const int64_t max_lsb = 1LL << log2_poc_lsb_;

    // A keyframe starts the count over; nothing before the first is repaired.
    if (s0.nal_type >= 16 && s0.nal_type <= 23) {
        have_prev_ = true;
        prev_poc_ = (s0.nal_type == 19 || s0.nal_type == 20) ? 0 : s0.poc_lsb;
        prev_us_ = us;
        return pass();
    }
    if (!have_prev_) return pass();

    // This picture's order count, from its lsb and the last picture's (8.3.1).
    const int64_t prev_lsb = prev_poc_ & (max_lsb - 1), prev_msb = prev_poc_ - prev_lsb;
    int64_t msb = prev_msb;
    if (s0.poc_lsb < prev_lsb && prev_lsb - s0.poc_lsb >= max_lsb / 2) msb += max_lsb;
    else if (s0.poc_lsb > prev_lsb && s0.poc_lsb - prev_lsb > max_lsb / 2) msb -= max_lsb;
    const int64_t poc = msb + s0.poc_lsb;

    // Lost pictures it refers to: one copy picture for each.
    const int64_t gap = poc - prev_poc_ - 1;
    if (gap > 0 && gap <= kMaxFill && s0.min_ref_delta < 0 && poc + s0.min_ref_delta > prev_poc_) {
        for (int64_t k = 1; k <= gap; k++) {
            const int64_t p = prev_poc_ + k;
            const uint64_t t = prev_us_ + (us > prev_us_ ? (us - prev_us_) * (uint64_t)k / (uint64_t)(gap + 1) : 0);
            out.push_back({copy_slice(1, (int)(p & (max_lsb - 1)), 0, ctbs_, nullptr), t});
            pics_filled_++;
        }
    }
    prev_poc_ = poc;
    prev_us_ = us;

    // Lost slices: the starts a whole picture has, as learned, against these.
    std::vector<int> have;
    for (const Slice& s : slices) have.push_back(s.addr);
    std::sort(have.begin(), have.end());
    if (have.front() == 0 && have.size() >= starts_.size()) starts_ = have;
    std::vector<int> want = starts_;
    want.insert(want.end(), have.begin(), have.end());
    want.push_back(0);
    std::sort(want.begin(), want.end());
    want.erase(std::unique(want.begin(), want.end()), want.end());
    struct Range { int a, b; };
    std::vector<Range> missing;
    for (size_t i = 0; i < want.size(); i++) {
        if (std::binary_search(have.begin(), have.end(), want[i])) continue;
        if (!missing.empty() && missing.back().b == want[i]) continue;   // already covered
        int b = ctbs_;
        for (int h : have) if (h > want[i]) { b = h; break; }
        missing.push_back({want[i], b});
    }
    if (missing.empty()) { out.push_back({au, us}); return out; }

    // The access unit again, slices in address order with the copies among them.
    std::vector<uint8_t> fixed(au.begin(), au.begin() + (long)s0.off);   // what precedes the slices
    std::vector<std::pair<int, std::vector<uint8_t>>> parts;
    for (const Slice& s : slices)
        parts.push_back({s.addr, std::vector<uint8_t>(au.begin() + (long)s.off, au.begin() + (long)(s.off + s.len))});
    for (const Range& r : missing) {
        parts.push_back({r.a, copy_slice(s0.nal_type, s0.poc_lsb, r.a, r.b, &s0)});
        slices_filled_++;
    }
    std::stable_sort(parts.begin(), parts.end(),
                     [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& p : parts) fixed.insert(fixed.end(), p.second.begin(), p.second.end());
    out.push_back({std::move(fixed), us});
    return out;
}

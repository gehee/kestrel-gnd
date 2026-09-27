#include "h264_cfr.hpp"

#include <cstdio>

namespace {

// --- RBSP <-> NAL payload (emulation prevention) --------------------------

std::vector<uint8_t> unescape(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && p[i] == 0x03) { zeros = 0; continue; }
        out.push_back(p[i]);
        zeros = p[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

void escape_into(std::vector<uint8_t>& out, const std::vector<uint8_t>& rbsp) {
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 0x03) { out.push_back(0x03); zeros = 0; }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

// --- bits -----------------------------------------------------------------

struct BitReader {
    const std::vector<uint8_t>& d;
    size_t pos = 0;   // in bits
    bool over = false;
    explicit BitReader(const std::vector<uint8_t>& v) : d(v) {}
    unsigned u(unsigned n) {
        unsigned v = 0;
        for (unsigned i = 0; i < n; i++) {
            if (pos >= d.size() * 8) { over = true; return 0; }
            v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    unsigned ue() {
        unsigned zeros = 0;
        while (!over && u(1) == 0) { if (++zeros > 31) { over = true; return 0; } }
        return zeros ? ((1u << zeros) - 1 + u(zeros)) : 0;
    }
    int se() {
        unsigned k = ue();
        return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
    }
};

struct BitWriter {
    std::vector<uint8_t> d;
    unsigned bits = 0;   // bits used in the last byte
    void u(unsigned n, unsigned v) {
        for (int i = (int)n - 1; i >= 0; i--) {
            if (bits == 0) d.push_back(0);
            d.back() |= ((v >> i) & 1) << (7 - bits);
            bits = (bits + 1) & 7;
        }
    }
    void ue(unsigned v) {
        unsigned x = v + 1, len = 0;
        for (unsigned t = x; t > 1; t >>= 1) len++;
        u(len, 0);
        u(len + 1, x);
    }
    void se(int v) { ue(v > 0 ? 2 * (unsigned)v - 1 : 2 * (unsigned)(-v)); }
    void trailing() { u(1, 1); while (bits) u(1, 0); }
};

// rangeTabLPS[pStateIdx][qCodIRangeIdx] and transIdxLPS, H.264 tables 9-44
// and 9-45 (values as in FFmpeg's libavcodec/cabac.c).
const uint8_t kRangeLPS[64][4] = {
    {128,176,208,240}, {128,167,197,227}, {128,158,187,216}, {123,150,178,205},
    {116,142,169,195}, {111,135,160,185}, {105,128,152,175}, {100,122,144,166},
    { 95,116,137,158}, { 90,110,130,150}, { 85,104,123,142}, { 81, 99,117,135},
    { 77, 94,111,128}, { 73, 89,105,122}, { 69, 85,100,116}, { 66, 80, 95,110},
    { 62, 76, 90,104}, { 59, 72, 86, 99}, { 56, 69, 81, 94}, { 53, 65, 77, 89},
    { 51, 62, 73, 85}, { 48, 59, 69, 80}, { 46, 56, 66, 76}, { 43, 53, 63, 72},
    { 41, 50, 59, 69}, { 39, 48, 56, 65}, { 37, 45, 54, 62}, { 35, 43, 51, 59},
    { 33, 41, 48, 56}, { 32, 39, 46, 53}, { 30, 37, 43, 50}, { 29, 35, 41, 48},
    { 27, 33, 39, 45}, { 26, 31, 37, 43}, { 24, 30, 35, 41}, { 23, 28, 33, 39},
    { 22, 27, 32, 37}, { 21, 26, 30, 35}, { 20, 24, 29, 33}, { 19, 23, 27, 31},
    { 18, 22, 26, 30}, { 17, 21, 25, 28}, { 16, 20, 23, 27}, { 15, 19, 22, 25},
    { 14, 18, 21, 24}, { 14, 17, 20, 23}, { 13, 16, 19, 22}, { 12, 15, 18, 21},
    { 12, 14, 17, 20}, { 11, 14, 16, 19}, { 11, 13, 15, 18}, { 10, 12, 15, 17},
    { 10, 12, 14, 16}, {  9, 11, 13, 15}, {  9, 11, 12, 14}, {  8, 10, 12, 14},
    {  8,  9, 11, 13}, {  7,  9, 11, 12}, {  7,  9, 10, 12}, {  7,  8, 10, 11},
    {  6,  8,  9, 11}, {  6,  7,  9, 10}, {  6,  7,  8,  9}, {  2,  2,  2,  2},
};
const uint8_t kTransLPS[64] = {
     0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9, 11, 11, 12,
    13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
    24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
    33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63,
};

// The CABAC encoding engine, H.264 9.3.4 - just enough of it to code the one
// context a skip slice uses, and the terminating bin.
struct Cabac {
    BitWriter& w;
    unsigned low = 0, range = 510, outstanding = 0;
    bool first = true;
    explicit Cabac(BitWriter& bw) : w(bw) {}
    void put(unsigned b) {
        if (first) first = false; else w.u(1, b);
        for (; outstanding; outstanding--) w.u(1, 1 - b);
    }
    void renorm() {
        while (range < 256) {
            if (low < 256) put(0);
            else if (low >= 512) { low -= 512; put(1); }
            else { low -= 256; outstanding++; }
            range <<= 1;
            low <<= 1;
        }
    }
    void decision(uint8_t& state, uint8_t& mps, unsigned bin) {
        unsigned lps = kRangeLPS[state][(range >> 6) & 3];
        range -= lps;
        if (bin != mps) {
            low += range;
            range = lps;
            if (state == 0) mps = 1 - mps;
            state = kTransLPS[state];
        } else if (state < 62) {
            state++;
        }
        renorm();
    }
    void terminate(unsigned bin) {
        range -= 2;
        if (bin) {
            low += range;
            // Flush. Its last bit is the slice's rbsp_stop_one_bit.
            range = 2;
            renorm();
            put((low >> 9) & 1);
            w.u(2, ((low >> 7) & 3) | 1);
        } else {
            renorm();
        }
    }
};

// Overwrite `n` bits at bit position `pos` in place.
void put_bits(std::vector<uint8_t>& d, size_t pos, unsigned n, unsigned v) {
    for (unsigned i = 0; i < n; i++, pos++) {
        uint8_t mask = (uint8_t)(0x80 >> (pos & 7));
        if ((v >> (n - 1 - i)) & 1) d[pos >> 3] |= mask;
        else                        d[pos >> 3] &= (uint8_t)~mask;
    }
}

// Annex-B -> (offset, length) of each NAL payload, start codes excluded.
std::vector<std::pair<size_t, size_t>> split_nals(const uint8_t* p, size_t n) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                if (end > start && p[end - 1] == 0) end--;   // 4-byte start code
                out.push_back({start, end - start});
            }
            i += 3;
            start = i;
            continue;
        }
        i++;
    }
    if (start != SIZE_MAX && start < n) out.push_back({start, n - start});
    return out;
}

}  // namespace

bool H264Cfr::init(const std::vector<uint8_t>& sps_pps) {
    ok_ = false;
    bool have_sps = false, have_pps = false;
    for (auto& nal : split_nals(sps_pps.data(), sps_pps.size())) {
        const uint8_t* p = sps_pps.data() + nal.first;
        if (nal.second < 2) continue;
        int type = p[0] & 0x1F;
        std::vector<uint8_t> rbsp = unescape(p + 1, nal.second - 1);
        BitReader b(rbsp);
        if (type == 7) {
            unsigned profile = b.u(8);
            b.u(8); b.u(8);          // constraint flags, level
            b.ue();                  // sps id
            static const unsigned high[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
            for (unsigned h : high) if (profile == h) {
                if (b.ue() == 3) b.u(1);             // chroma_format_idc, separate planes
                b.ue(); b.ue(); b.u(1);              // bit depths, qpprime bypass
                if (b.u(1)) { printf("cfr: SPS scaling matrices - not handled\n"); return false; }
            }
            log2_max_frame_num_ = b.ue() + 4;
            poc_type_ = b.ue();
            if (poc_type_ == 0) log2_max_poc_lsb_ = b.ue() + 4;
            else if (poc_type_ == 1) { printf("cfr: POC type 1 - not handled\n"); return false; }
            b.ue(); b.u(1);                          // max_num_ref_frames, gaps allowed
            unsigned w = b.ue() + 1, h = b.ue() + 1;
            if (!b.u(1)) { printf("cfr: interlaced SPS - not handled\n"); return false; }
            mb_count_ = w * h;
            have_sps = !b.over;
        } else if (type == 8) {
            pps_id_ = b.ue();
            b.ue();                                  // sps id
            cabac_ = b.u(1);
            bottom_field_poc_present_ = b.u(1);
            if (b.ue()) { printf("cfr: slice groups - not handled\n"); return false; }
            b.ue(); b.ue();                          // default ref idx counts
            if (b.u(1)) { printf("cfr: weighted prediction - not handled\n"); return false; }
            b.u(2);                                  // weighted bipred
            slice_qp_ = 26 + b.se();                 // pic_init_qp_minus26
            b.se(); b.se();                          // qs, chroma offset
            deblocking_control_present_ = b.u(1);
            b.u(1);                                  // constrained intra
            redundant_pic_cnt_present_ = b.u(1);
            have_pps = !b.over;
        }
    }
    ok_ = have_sps && have_pps && mb_count_ > 0;
    if (ok_)
        printf("cfr: %u macroblocks, %s, frame_num %u bits, POC type %u\n", mb_count_,
               cabac_ ? "CABAC" : "CAVLC", log2_max_frame_num_, poc_type_);
    return ok_;
}

void H264Cfr::rewrite_slice(std::vector<uint8_t>& rbsp, int nal_type) {
    BitReader b(rbsp);
    unsigned first_mb = b.ue();
    b.ue();                                  // slice_type
    b.ue();                                  // pps id
    size_t fn_pos = b.pos;
    unsigned frame_num = b.u(log2_max_frame_num_);
    if (nal_type == 5) b.ue();               // idr_pic_id
    size_t poc_pos = b.pos;
    unsigned poc = poc_type_ == 0 ? b.u(log2_max_poc_lsb_) : 0;
    if (b.over) return;

    const unsigned fn_mask  = (1u << log2_max_frame_num_) - 1;
    const unsigned poc_mask = (1u << log2_max_poc_lsb_) - 1;

    if (nal_type == 5) {
        // A fresh start: nothing to shift until the next skip frame.
        num_shift_ = 0;
        if (first_mb == 0) {
            last_frame_num_ = frame_num;
            last_poc_lsb_   = poc;
            have_frame_     = true;
        }
        return;
    }

    unsigned fn2  = (frame_num + num_shift_) & fn_mask;
    unsigned poc2 = (poc + num_shift_ * poc_step_) & poc_mask;
    put_bits(rbsp, fn_pos, log2_max_frame_num_, fn2);
    if (poc_type_ == 0) {
        // The encoder's own POC step, learnt from its first P frame after an
        // IDR, is what a skip frame advances by.
        if (first_mb == 0 && have_frame_ && num_shift_ == 0) {
            unsigned step = (poc - last_poc_lsb_) & poc_mask;
            if (step > 0 && step < 16) poc_step_ = step;
        }
        put_bits(rbsp, poc_pos, log2_max_poc_lsb_, poc2);
    }
    if (first_mb == 0) {
        last_frame_num_ = fn2;
        last_poc_lsb_   = poc2;
        have_frame_     = true;
    }
}

std::vector<uint8_t> H264Cfr::rewrite(const uint8_t* au, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len + 16);
    for (auto& nal : split_nals(au, len)) {
        const uint8_t* p = au + nal.first;
        if (nal.second == 0) continue;
        int type = p[0] & 0x1F;
        out.insert(out.end(), {0, 0, 0, 1});
        if (ok_ && (type == 1 || type == 5) && nal.second > 1) {
            std::vector<uint8_t> rbsp = unescape(p + 1, nal.second - 1);
            rewrite_slice(rbsp, type);
            out.push_back(p[0]);
            escape_into(out, rbsp);
        } else {
            out.insert(out.end(), p, p + nal.second);
        }
    }
    return out;
}

std::vector<uint8_t> H264Cfr::skip_frame() {
    std::vector<uint8_t> out;
    if (!ok_ || !have_frame_) return out;
    const unsigned fn_mask  = (1u << log2_max_frame_num_) - 1;
    const unsigned poc_mask = (1u << log2_max_poc_lsb_) - 1;
    unsigned fn  = (last_frame_num_ + 1) & fn_mask;
    unsigned poc = (last_poc_lsb_ + poc_step_) & poc_mask;

    BitWriter w;
    w.ue(0);                         // first_mb_in_slice
    w.ue(5);                         // slice_type: P, and every slice of the picture P
    w.ue(pps_id_);
    w.u(log2_max_frame_num_, fn);
    if (poc_type_ == 0) {
        w.u(log2_max_poc_lsb_, poc);
        if (bottom_field_poc_present_) w.se(0);
    }
    if (redundant_pic_cnt_present_) w.ue(0);
    w.u(1, 0);                       // num_ref_idx_active_override_flag
    w.u(1, 0);                       // ref_pic_list_modification_flag_l0
    w.u(1, 0);                       // adaptive_ref_pic_marking_mode_flag (sliding window)
    if (cabac_) w.ue(0);             // cabac_init_idc
    w.se(0);                         // slice_qp_delta
    if (deblocking_control_present_) w.ue(1);   // no deblocking: nothing to filter
    if (!cabac_) {
        w.ue(mb_count_);             // mb_skip_run: every macroblock skipped
        w.trailing();
    } else {
        while (w.bits) w.u(1, 1);    // cabac_alignment_one_bit
        // mb_skip_flag uses ctxIdx 11..13 by how many neighbours were coded;
        // every neighbour here is skipped, so it is 11 throughout. Its
        // initial state for cabac_init_idc 0 is (m, n) = (23, 33).
        int qp = slice_qp_ < 0 ? 0 : slice_qp_ > 51 ? 51 : slice_qp_;
        int pre = ((23 * qp) >> 4) + 33;
        pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
        uint8_t state = pre <= 63 ? (uint8_t)(63 - pre) : (uint8_t)(pre - 64);
        uint8_t mps   = pre <= 63 ? 0 : 1;
        Cabac c(w);
        for (unsigned i = 0; i < mb_count_; i++) {
            c.decision(state, mps, 1);        // mb_skip_flag
            c.terminate(i + 1 == mb_count_);  // end_of_slice_flag
        }
        while (w.bits) w.u(1, 0);    // rbsp_alignment_zero_bit
    }

    out.insert(out.end(), {0, 0, 0, 1, 0x41});  // non-IDR slice, nal_ref_idc 2
    escape_into(out, w.d);

    last_frame_num_ = fn;
    last_poc_lsb_   = poc;
    num_shift_++;
    return out;
}

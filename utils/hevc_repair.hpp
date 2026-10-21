#pragma once
// A recording of the air unit's H.265 stream, repaired where the radio lost
// part of it - without a keyframe asked for and without re-encoding.
//
// With intra refresh every picture refers to the one before it. A picture the
// radio lost leaves the next one referring to nothing: FFmpeg conceals that,
// but Apple's decoder (QuickTime, Photos, iOS) stops there and waits for a
// keyframe that never comes - the recording froze from the first loss on.
//
// Each lost picture is replaced by a copy picture: a P picture with the lost
// one's picture order count, every block of it a skip with no motion (no
// temporal motion candidates, no SAO), which decodes to the picture before it
// exactly. The pictures after it then find what they refer to. A picture that
// lost one of its slices gets a copy slice over the missing area instead (with
// the picture's own reference set).
//
// Only what the air unit's encoder uses is handled: no tiles, no wavefronts,
// no dependent slices, no weighted prediction, no scaling lists in the
// parameter sets. Anything else turns the repair off and the stream passes
// through untouched.
#include <cstdint>
#include <vector>

class HevcRepair {
public:
    // The stream's VPS/SPS/PPS, Annex-B (each with its start code) or bare.
    void set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps);
    // A new recording: nothing is repaired before its first keyframe.
    void reset();

    struct Unit {
        std::vector<uint8_t> au;   // Annex-B access unit
        uint64_t us;               // its time (copy pictures: between their neighbours)
    };
    // One access unit (Annex-B) at time us. Returns what to write in its
    // place, in order: copy pictures for lost ones, then the unit itself (with
    // copy slices for lost slices). Untouched when the repair is off.
    std::vector<Unit> process(const std::vector<uint8_t>& au, uint64_t us);

    bool active() const { return ok_; }
    unsigned pictures_filled() const { return pics_filled_; }
    unsigned slices_filled() const { return slices_filled_; }

private:
    struct Slice {
        size_t off = 0, len = 0;   // the NAL in the access unit, start code included
        int nal_type = 0;
        int addr = 0;              // first CTB
        int slice_type = 0;
        int poc_lsb = 0;
        bool tmvp = false;
        // The reference picture set as coded (short_term_ref_pic_set_sps_flag
        // up to the long-term part's end), bits of the RBSP.
        std::vector<uint8_t> rbsp;
        int rps_bit0 = 0, rps_bit1 = 0;
        int min_ref_delta = 0;     // the nearest picture it refers to, as a POC delta (< 0)
    };
    bool parse_slice(const uint8_t* nal, size_t len, Slice& s) const;
    std::vector<uint8_t> copy_slice(int nal_type, int poc_lsb, int addr, int end,
                                    const Slice* rps_from) const;

    // SPS / PPS, what the copies need.
    bool ok_ = false;
    int width_ = 0, height_ = 0, log2_ctb_ = 0, log2_min_cb_ = 0;
    int ctbs_w_ = 0, ctbs_h_ = 0, ctbs_ = 0, addr_bits_ = 0;
    int log2_poc_lsb_ = 0, num_st_rps_ = 0, num_lt_sps_ = 0, lt_lsb_bits_ = 0;
    bool chroma_ = true, sao_ = false, lt_present_ = false, sps_tmvp_ = false;
    std::vector<std::vector<int>> sps_rps_neg_;   // each SPS set's negative deltas
    int pps_id_ = 0, num_extra_bits_ = 0, num_ref_l0_default_ = 1, init_qp_ = 26;
    bool output_flag_ = false, cabac_init_present_ = false, chroma_qp_offsets_ = false;
    bool transquant_bypass_ = false, lf_across_slices_ = false;
    bool deblock_override_ = false, pps_deblock_disabled_ = false, slice_ext_ = false;

    // The recording's last picture.
    bool have_prev_ = false;
    int64_t prev_poc_ = 0;
    uint64_t prev_us_ = 0;
    std::vector<int> starts_;      // slice starts of a whole picture, as learned
    unsigned pics_filled_ = 0, slices_filled_ = 0;
};

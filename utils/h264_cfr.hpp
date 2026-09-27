#ifndef H264_CFR_HPP
#define H264_CFR_HPP

// Constant frame rate for the screen recording.
//
// The display scans out at its refresh rate whether or not anything new was
// committed; a scan-out with no new commit shows the previous picture again.
// The recording should run at that same rate, so that it plays back exactly
// as the goggle looked. Encoding the held picture again costs a full encode
// per scan-out, and the VEPU is rated for 1080p60 - so instead, each held
// scan-out becomes a P frame in which every macroblock is skipped: "same as
// the last picture", a few bytes, no encoder involved.
//
// Those frames are reference frames, so every frame the encoder produces
// after one has its frame_num (and, for POC type 0, pic_order_cnt_lsb)
// shifted along to make room. Both are fixed-width fields in the slice
// header, so they are rewritten in place; the CABAC or CAVLC data behind
// them is untouched. An IDR resets both counters and the shift with them.
//
// Skip slices are written in whichever entropy coding the encoder's PPS
// declares. With CAVLC a run of skipped macroblocks is one Exp-Golomb number;
// with CABAC (what the VEPU is run with - it hangs with CABAC off) each
// macroblock's skip flag and end-of-slice flag go through a small arithmetic
// coder here.

#include <cstddef>
#include <cstdint>
#include <vector>

class H264Cfr {
    public:
        // Parse the encoder's SPS/PPS (Annex-B). False if the stream uses
        // something this does not handle (interlace, POC type 1, weighted
        // prediction, several slice groups) - the recording then simply
        // stays variable-rate.
        bool init(const std::vector<uint8_t>& sps_pps);
        bool ready() const { return ok_; }

        // One encoded access unit (Annex-B, SPS/PPS/SEI/slices). Returns it
        // with every slice renumbered for the skip frames inserted so far.
        std::vector<uint8_t> rewrite(const uint8_t* au, size_t len);

        // A skip frame repeating the last picture, as one Annex-B NAL.
        // Call only after at least one frame has gone through rewrite().
        std::vector<uint8_t> skip_frame();

    private:
        bool ok_ = false;
        // SPS
        unsigned log2_max_frame_num_ = 4;
        unsigned poc_type_ = 0;
        unsigned log2_max_poc_lsb_ = 4;
        unsigned mb_count_ = 0;
        // PPS
        unsigned pps_id_ = 0;
        bool bottom_field_poc_present_ = false;
        bool redundant_pic_cnt_present_ = false;
        bool deblocking_control_present_ = false;
        bool cabac_ = false;
        int  slice_qp_ = 26;            // 26 + pic_init_qp_minus26
        // Running state: what the last frame out carried, and the shift.
        bool     have_frame_ = false;
        unsigned last_frame_num_ = 0;   // after the shift
        unsigned last_poc_lsb_   = 0;   // after the shift
        unsigned num_shift_ = 0;        // skip frames since the last IDR
        unsigned poc_step_  = 2;        // POC gap between two encoder frames

        void rewrite_slice(std::vector<uint8_t>& rbsp, int nal_type);
};

#endif

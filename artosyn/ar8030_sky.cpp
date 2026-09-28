#include "ar8030_sky.hpp"

#include <cstring>
#include <sys/time.h>

namespace sky {

// The 120 fps entries are labelled 100: 120 is the value stock sends on the
// wire and what the menu matches against, but the air unit does not support 120.
const VideoMode kFpvModes[] = {
    { "720p60",   1280,  720,  60 },
    { "720p100",  1280,  720, 120 },
    { "1080p60",  1920, 1080,  60 },
    { "1080p100", 1920, 1080, 120 },
};
const int kFpvModeCount = (int)(sizeof(kFpvModes) / sizeof(kFpvModes[0]));

int default_fpv_mode_index() {
    for (int i = 0; i < kFpvModeCount; i++)
        if (kFpvModes[i].w == 1920 && kFpvModes[i].h == 1080 && kFpvModes[i].fps == 120)
            return i;
    return 0;
}


// Field offsets within the 59-byte SET_CONFIG body (section 21.5).
namespace {
enum : size_t {
    OFF_TV_SEC   = 1,   // LE32, live gettimeofday on stock
    OFF_TV_USEC  = 5,   // LE32
    OFF_EV       = 9,
    OFF_SCENES   = 10,
    OFF_SAT      = 11,
    OFF_SHARP    = 12,
    OFF_AWB      = 13,  // LE16
    OFF_ANGLE    = 15,
    OFF_RATIO    = 16,
    OFF_CH0_W    = 17,  // LE16
    OFF_CH0_H    = 19,  // LE16
    OFF_CH0_FPS  = 21,  // u8 - fps is NOT 16-bit here
    OFF_FOCUS    = 22,  // sits between the two channels; not padding
    OFF_CH1_W    = 23,
    OFF_CH1_H    = 25,
    OFF_CH1_FPS  = 27,
    OFF_3DNR     = 35,  // read-only: the air's echo of the 3D DNR level it was
                        // given via cmd 0x1B. Writing it here is acked and ignored.
    OFF_FLICKER  = 44,
    OFF_STRAT_CON= 47,  // low nibble = video_strategy, high nibble = contrast
    CONFIG_LEN   = 59,
};
inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline uint16_t get16(const uint8_t *p)   { return (uint16_t)(p[0] | (p[1] << 8)); }
inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
}  // namespace

bool patch_config_payload(uint8_t *payload, size_t len, const SkyConfig &c) {
    if (!payload || len < CONFIG_LEN) return false;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    put32(payload + OFF_TV_SEC,  (uint32_t)tv.tv_sec);
    put32(payload + OFF_TV_USEC, (uint32_t)tv.tv_usec);
    payload[OFF_EV]     = (uint8_t)c.ev_x10;
    payload[OFF_SCENES] = c.scenes;
    payload[OFF_SAT]    = c.saturation;
    payload[OFF_SHARP]  = c.sharpness;
    put16(payload + OFF_AWB, c.awb_cct);
    payload[OFF_ANGLE]  = c.angle;
    payload[OFF_RATIO]  = c.image_ratio;
    put16(payload + OFF_CH0_W, c.ch0_w);
    put16(payload + OFF_CH0_H, c.ch0_h);
    payload[OFF_CH0_FPS] = c.ch0_fps;
    payload[OFF_FOCUS]   = c.focus_en;
    put16(payload + OFF_CH1_W, c.ch1_w);
    put16(payload + OFF_CH1_H, c.ch1_h);
    payload[OFF_CH1_FPS] = c.ch1_fps;
    payload[OFF_FLICKER] = c.anti_flicker;
    payload[OFF_STRAT_CON] = (uint8_t)((c.video_strategy & 0x0F) | ((c.contrast & 0x0F) << 4));
    return true;
}

bool read_config_payload(const uint8_t *payload, size_t len, SkyConfig *o) {
    if (!payload || !o || len < CONFIG_LEN) return false;
    o->ev_x10      = (int8_t)payload[OFF_EV];
    o->scenes      = payload[OFF_SCENES];
    o->saturation  = payload[OFF_SAT];
    o->sharpness   = payload[OFF_SHARP];
    o->awb_cct     = get16(payload + OFF_AWB);
    o->angle       = payload[OFF_ANGLE];
    o->image_ratio = payload[OFF_RATIO];
    o->ch0_w = get16(payload + OFF_CH0_W);
    o->ch0_h = get16(payload + OFF_CH0_H);
    o->ch0_fps = payload[OFF_CH0_FPS];
    o->focus_en = payload[OFF_FOCUS];
    o->ch1_w = get16(payload + OFF_CH1_W);
    o->ch1_h = get16(payload + OFF_CH1_H);
    o->ch1_fps = payload[OFF_CH1_FPS];
    o->dnr_3d       = payload[OFF_3DNR];
    o->anti_flicker = payload[OFF_FLICKER];
    o->video_strategy = (uint8_t)(payload[OFF_STRAT_CON] & 0x0F);
    o->contrast       = (uint8_t)(payload[OFF_STRAT_CON] >> 4);
    return true;
}

uint32_t Proto::crc32c(const uint8_t* d, size_t n) {
    // Reflected CRC-32C with init = 0 and no final xor. Recovered by fitting
    // the parameter space against the captured frames; the usual 0xFFFFFFFF
    // init/xorout does not match.
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
    }
    return c;
}

std::vector<uint8_t> Proto::build(uint8_t cmd, const uint8_t* payload, size_t len) {
    const uint16_t lf = (uint16_t)((len + 1) << 4);   // cmd byte counts toward len
    const uint8_t lo = (uint8_t)(lf & 0xFF), hi = (uint8_t)(lf >> 8);
    const uint8_t s = seq_;
    seq_ = (uint8_t)(seq_ + 1);
    if (seq_ == 0) seq_ = 1;                          // stock never sends seq 0

    std::vector<uint8_t> f;
    f.reserve(11 + len);
    f.push_back(0xFE);
    f.push_back(0xA5);
    f.push_back((uint8_t)(s + lo + hi));              // header checksum
    f.push_back(s);
    f.push_back(lo);
    f.push_back(hi);
    f.push_back(cmd);
    if (payload && len) f.insert(f.end(), payload, payload + len);
    f.push_back(0x00);                                // pad the stock framer appends

    const uint32_t c = crc32c(f.data(), f.size());
    f.push_back((uint8_t)(c      ));
    f.push_back((uint8_t)(c >>  8));
    f.push_back((uint8_t)(c >> 16));
    f.push_back((uint8_t)(c >> 24));
    return f;
}

std::vector<uint8_t> Proto::build_u32(uint8_t cmd, uint32_t v) {
    const uint8_t p[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                           (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return build(cmd, p, 4);
}

std::vector<uint8_t> Proto::set_chn_res(uint8_t chn, uint16_t w, uint16_t h, uint8_t fps) {
    // Exactly the 6-byte buffer ar_ldy_gnd assembles for GUI_CMD_SET_CHN_RES.
    const uint8_t p[6] = { chn,
                           (uint8_t)(w & 0xFF), (uint8_t)(w >> 8),
                           (uint8_t)(h & 0xFF), (uint8_t)(h >> 8),
                           fps };
    return build(CMD_SET_CHN_RES, p, 6);
}

std::vector<uint8_t> Proto::set_bb_freq(bool hop_en, uint8_t slot, uint32_t freq_khz) {
    // The 6-byte buffer ar_ldy_gnd serialises at 0x88f14 for
    // GUI_CMD_SET_BB_FREQ, then hands to send_sky_cmd(0x21, buf, 6).
    const uint8_t p[6] = { (uint8_t)(hop_en ? 1 : 0), slot,
                           (uint8_t)(freq_khz         & 0xFF),
                           (uint8_t)((freq_khz >>  8) & 0xFF),
                           (uint8_t)((freq_khz >> 16) & 0xFF),
                           (uint8_t)((freq_khz >> 24) & 0xFF) };
    return build(CMD_SET_BB_FREQ, p, 6);
}

std::vector<uint8_t> Proto::set_work_chan_list(const uint32_t *freq_khz, size_t n) {
    // ar_ldy_gnd:0x89318 - send_sky_cmd(0x29, freqs, count * 4). Little-endian
    // u32 per channel, no count field: the length carries it.
    std::vector<uint8_t> p;
    p.reserve(n * 4);
    for (size_t i = 0; i < n; i++) {
        p.push_back((uint8_t)( freq_khz[i]        & 0xFF));
        p.push_back((uint8_t)((freq_khz[i] >>  8) & 0xFF));
        p.push_back((uint8_t)((freq_khz[i] >> 16) & 0xFF));
        p.push_back((uint8_t)((freq_khz[i] >> 24) & 0xFF));
    }
    return build(CMD_SET_WORK_CHAN_LIST, p.data(), p.size());
}

int Proto::find_tlv(const uint8_t* payload, size_t len, uint8_t tag) {
    if (len <= kStatusTlvOffset) return -1;
    size_t i = kStatusTlvOffset;
    while (i + 2 < len) {
        uint8_t l = payload[i];
        if (l < 3 || i + l > len) break;   // malformed or padding: stop
        if (payload[i + 1] == tag) return payload[i + 2];
        i += l;
    }
    return -1;
}

bool Proto::parse(const uint8_t* buf, size_t len,
                  uint8_t* cmd, bool* is_ack, uint8_t* status) {
    if (len < 12 || buf[0] != 0xFE || buf[1] != 0xA5) return false;
    const uint16_t lf = (uint16_t)(buf[4] | (buf[5] << 8));
    const size_t n = lf >> 4;                        // cmd + payload
    if (n < 1 || 6 + n + 1 + 4 != len) return false;
    if ((uint8_t)(buf[3] + buf[4] + buf[5]) != buf[2]) return false;
    const uint32_t want = (uint32_t)buf[len - 4]          |
                          ((uint32_t)buf[len - 3] <<  8)  |
                          ((uint32_t)buf[len - 2] << 16)  |
                          ((uint32_t)buf[len - 1] << 24);
    if (crc32c(buf, len - 4) != want) return false;

    if (cmd)    *cmd    = buf[6];
    if (is_ack) *is_ack = (lf & 0xF) == 8;
    if (status) *status = (n >= 2) ? buf[7] : 0;
    return true;
}

}  // namespace sky

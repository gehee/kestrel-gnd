#include "msp_osd.hpp"
#include "stab/imu_stream.hpp"
#include "utils/time_util.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#include <cctype>
#include <cmath>

// CRC8 lookup table for MSP V2 (same as TX side)
static uint8_t crc8_dvb_s2(uint8_t crc, unsigned char a) {
    crc ^= a;
    for (int i = 0; i < 8; ++i) {
        if (crc & 0x80)
            crc = (crc << 1) ^ 0xD5;
        else
            crc = (crc << 1);
    }
    return crc;
}

MspOsd::MspOsd() {
    clear();
}

void MspOsd::clear() {
    std::lock_guard<std::mutex> lock(mtx);
    clear_internal();
}

void MspOsd::clear_internal() {
    // Only clear pending grid
    for(int y=0; y<GRID_H; y++) {
        for(int x=0; x<GRID_W; x++) {
            pending_grid[y][x].char_idx = 0; // Empty
            pending_grid[y][x].attr = 0;
        }
    }
}

void MspOsd::parse_bytes(const uint8_t* p, size_t size) {
    for(size_t i=0; i<size; i++) {
        parse_byte(p[i]);
    }
}

void MspOsd::parse_byte(uint8_t b) {
    switch (state) {
        case 0: // $
            if (b == '$') state = 1;
            break;
        case 1: // X (V2) or M (V1)
            if (b == 'X') {
                state = 30;
                crc = 0;
                is_v2 = true;
            } else if (b == 'M') {
                state = 20;
                checksum_v1 = 0;
                is_v2 = false;
            } else {
                state = 0;
            }
            break;
            
        // MSP v2 carries a direction byte ('<' request, '>' reply, '!' error)
        // between the 'X' and the flag. Consuming it as the flag shifted every
        // later field by one byte - function, length, payload end and CRC - so
        // the CRC could never match and the v2 path silently accepted nothing.
        case 30: // Direction
            if (b == '<' || b == '>' || b == '!') {
                crc = 0;
                state = 2;
            } else {
                state = 0;
            }
            break;

        // --- MSP V2 States ---
        case 2: // Flag
            flags = b;
            crc = crc8_dvb_s2(crc, b);
            state = 3;
            break;
        case 3: // Function L
            function = b;
            crc = crc8_dvb_s2(crc, b);
            state = 4;
            break;
        case 4: // Function H
            function |= (b << 8);
            crc = crc8_dvb_s2(crc, b);
            state = 5;
            break;
        case 5: // Size L
            payload_size = b;
            crc = crc8_dvb_s2(crc, b);
            state = 6;
            break;
        case 6: // Size H
            payload_size |= (b << 8);
            crc = crc8_dvb_s2(crc, b);
            payload_buf.clear();
            payload_buf.reserve(payload_size);
            if (payload_size == 0) state = 8; // No payload
            else state = 7;
            break;
        case 7: // Payload
            payload_buf.push_back(b);
            crc = crc8_dvb_s2(crc, b);
            if (payload_buf.size() == payload_size) {
                state = 8;
            }
            break;
        case 8: // CRC
            if (crc == b) {
                handle_msp_frame(function, payload_buf.data(), payload_size);
            } else {
               // printf("MSP V2 CRC Error\n");
            }
            state = 0;
            break;

        // --- MSP V1 States ---
        case 20: // Direction ('>' reply, '<' unsolicited/request)
            if (b == '>' || b == '<') {
                state = 21;
            } else {
                state = 0;
            }
            break;
        case 21: // Size
            payload_size = b;
            checksum_v1 ^= b;
            state = 22;
            break;
        case 22: // Type (Function)
            function = b;
            checksum_v1 ^= b;
            payload_buf.clear();
            payload_buf.reserve(payload_size);
            if (payload_size == 0) state = 24;
            else state = 23;
            break;
        case 23: // Payload
            payload_buf.push_back(b);
            checksum_v1 ^= b;
            if (payload_buf.size() == payload_size) {
                state = 24;
            }
            break;
        case 24: // Checksum
            if (checksum_v1 == b) {
                handle_msp_frame(function, payload_buf.data(), payload_size);
            } else {
                // printf("MSP V1 Checksum Error\n");
            }
            state = 0;
            break;

        default:
            state = 0;
            break;
    }
}

// // void MspOsd::handle_msp_frame(uint16_t function, const uint8_t* payload, size_t size) {
// The AR8030 air unit does not forward Betaflight's DisplayPort verbatim. It
// re-encodes a whole screen refresh into one MSP v2 frame on function 182:
//
//   payload: 35 <seq_lo> <seq_hi>  then records, back to back:
//            [total][row][col][attr][chars ...]      total includes the 4 hdr bytes
//
// Verified against a live capture off the port-2 telemetry socket: 651/651
// frames parse to exactly the payload end, rows land in 11..19 and columns in
// 0..47 of the 53x20 canvas, and the result renders as a coherent Betaflight
// OSD (DISARMED / PID_1 / RATE_1 / LAND NOW / cell + pack voltage / mAh).
// Strings carry OSD glyph indices below 0x20 as well as ASCII.
//
// Returns false if the payload is not in this shape, so the caller can fall
// through to the stock one-subcommand-per-frame DisplayPort encoding.
bool MspOsd::handle_batched_displayport(const uint8_t* payload, size_t size) {
    if (size < 4 || payload[0] != 0x35) return false;

    std::lock_guard<std::mutex> lock(mtx);
    // No CLEAR subcommand exists in this encoding, but this is exactly why it
    // is safe to act as if every batch started with one: each frame is meant
    // to be a complete screen, so any cell the walk below does not reach -
    // because the payload ran out, or a record's own length disagreed with
    // what is actually there - is a cell the aircraft no longer wants shown,
    // not one to leave stale.
    clear_internal();

    // Applied as each record validates, not validated-then-applied: a
    // record's header (total/row/col) is checked before ITS OWN bytes are
    // ever written, so nothing garbled reaches the grid - but records
    // already confirmed good are kept even if a later one in the same frame
    // is not. This payload can arrive truncated, or - on a real frame, seen
    // regularly once a screen carries enough elements - spliced mid-record
    // with content that echoes the sky-protocol magic 0xFE 0xA5 used
    // elsewhere in this codebase (the air unit's own re-encoder, not
    // something wrong on this end). A full screen bar its last few elements
    // is a better outcome than a blank one, and there is nothing to guess
    // here - the walk stops at the first record it cannot trust rather than
    // scanning forward for a new sync point, which was tried once and risked
    // writing a garbled glyph into a cell nothing was actually wrong with.
    size_t i = 3;
    bool complete = true;
    while (i < size) {
        if (i + 4 > size) { complete = false; break; }
        uint8_t total = payload[i];
        if (total < 4 || i + total > size) { complete = false; break; }
        uint8_t row = payload[i + 1];
        uint8_t col = payload[i + 2];
        // payload[i + 3] is the attribute; only 0 and 0x43 were observed and
        // nothing downstream renders attributes yet.
        uint8_t len = (uint8_t)(total - 4);
        if (len) write_string_locked(row, col, payload + i + 4, len);
        i += total;
    }
    if (!complete) {
        static long truncated = 0;
        if ((++truncated % 200) == 1)
            fprintf(stderr, "[MSP-OSD] screen #%ld arrived incomplete (stopped at byte %zu of %zu) - "
                    "drawing what validated\n", truncated, i, size);
    }

    // Every frame is the refresh in this encoding, so commit immediately,
    // the same way DRAW_SCREEN does.
    if (memcmp(grid, pending_grid, sizeof(grid)) != 0) {
        memcpy(grid, pending_grid, sizeof(grid));
        content_ver_++;
    }
    scrape_locked();
    return true;
}

// --- Betaflight OSD scraping -------------------------------------------
//
// Glyph codes are the ones betaflight_glyphs.cpp already draws; see
// https://www.betaflight.com/docs/development/OSD-Glyps.
#define BF_SYM_RSSI      0x01
#define BF_SYM_LQ        0x02
#define BF_SYM_DISARMED  0x05
#define BF_SYM_VOLT      0x06
#define BF_SYM_AMP_A     0x08
#define BF_SYM_AMP_B     0x9A
#define BF_SYM_KMH       0x0A
#define BF_SYM_MPH       0x0B
#define BF_SYM_ALT_M     0x0C
// Betaflight's timer symbols live in the high range, not at 0x0F/0x10. The
// capture from this aircraft shows SYM_FLY_M (0x9C) beside its 00:00, which is
// why the flight timer was never found and, with it, why arm state stayed
// unknown and the arm animation never fired. SYM_LINK_QUALITY is 0x7B, an
// ASCII-range code, so it has to be matched before the printable-character
// path swallows it as a '{'.
#define BF_SYM_ON_M      0x9B
#define BF_SYM_FLY_M     0x9C
#define BF_SYM_LQ_ALT    0x7B
// Pitch/Roll Angle OSD elements: a marker glyph immediately followed by a
// signed one-decimal number, e.g. "<0x15>-01.4". Not part of the standard
// glyph set - identified empirically (see [[reactive-hud-attitude-source]]):
// isolating roll swung this glyph's value ~15 deg while the other barely
// moved, and free tilting swung it near 169 deg (rolling a handheld quad
// almost inverted) while the other stayed under 40 - so 0x14 is Roll,
// 0x15 is Pitch.
#define BF_SYM_ROLL      0x14
#define BF_SYM_PITCH     0x15

// Betaflight's own flight-mode strings (osdElementFlymode). Matching against
// the fixed list rather than "any four capitals" keeps craft names, warnings
// and the post-flight stats screen from being read as a mode.
static const char* kBfModes[] = {
    "!FS!", "!ERR", "RESC", "HEAD", "ANGL", "HOR ", "ATRN", "AIR ", "ACRO"
};

// Recover Pitch/Roll Angle straight off the raw MSP payload bytes, independent
// of handle_batched_displayport()'s record-boundary walk. Both elements
// render as a fixed 6-byte run - marker glyph, then a signed one-decimal
// number with no separators ("[space or '-'][digit][digit].[digit]") - so a
// plain scan finds them even in a frame whose record chain desyncs elsewhere
// (a splice from another buffer in the air's own re-encoder - see the comment
// on handle_batched_displayport). Only ever writes osd_pitch_deg/osd_roll_deg
// - never touches the grid - so a false match cannot corrupt anything
// visible; the sign/digit/dot check keeps false positives negligible in an
// 8-bit glyph-index stream.
void MspOsd::scan_osd_angles(const uint8_t* payload, size_t size) {
    bool have_pitch = false, have_roll = false;
    float pitch = 0.0f, roll = 0.0f;
    for (size_t k = 0; k + 4 <= size; k++) {
        uint8_t g = payload[k];
        if (g != BF_SYM_PITCH && g != BF_SYM_ROLL) continue;
        // [space or '-'] [1..3 digits] '.' [1 digit]. The integer part has to
        // be counted, not assumed to be two: roll runs to +/-180, and a craft
        // sitting at -161.4 matched nothing at all under a fixed two-digit
        // pattern - which, because both axes are required below, killed the
        // whole attitude path rather than just that one reading.
        size_t p = k + 1;
        bool neg = false;
        if (p < size && (payload[p] == ' ' || payload[p] == '-')) {
            neg = (payload[p] == '-');
            p++;
        }
        const size_t ds = p;
        while (p < size && isdigit(payload[p])) p++;
        const size_t ndig = p - ds;
        if (ndig < 1 || ndig > 3) continue;
        if (p >= size || payload[p] != '.') continue;
        p++;
        if (p >= size || !isdigit(payload[p])) continue;

        char buf[12];
        size_t n = 0;
        if (neg) buf[n++] = '-';
        for (size_t q = ds; q < ds + ndig; q++) buf[n++] = (char)payload[q];
        buf[n++] = '.';
        buf[n++] = (char)payload[p];
        buf[n]   = 0;
        float v = strtof(buf, nullptr);
        if (g == BF_SYM_PITCH) { pitch = v; have_pitch = true; }
        else                   { roll  = v; have_roll  = true; }
    }
    if (have_pitch && have_roll) {
        std::lock_guard<std::mutex> lock(mtx);
        // A new attitude moves the reactive HUD - news for the OSD loop, but
        // only when something on screen follows it, and only a change that
        // shows: 0.5 deg is about a pixel of HUD travel at MEDIUM. A craft
        // sitting still jitters by a tenth of a degree, which drawn at every
        // report kept the HUD rendering 30+ times a second for nothing.
        if (motion_wanted_ && (fabsf(pitch - osd_pitch_deg) >= 0.5f || fabsf(roll - osd_roll_deg) >= 0.5f))
            content_ver_++;
        osd_pitch_deg = pitch;
        osd_roll_deg  = roll;
        osd_angles_us = get_time_us();
    }
}

// Recover cell/pack voltage and flight mode straight off the raw MSP payload
// bytes, independent of handle_batched_displayport()'s record-boundary walk -
// same reasoning as scan_osd_angles above, and the reason voltage/mode were
// missing on the ground HUD while RSSI/amps/arm (all structured MSP, not
// DisplayPort) kept working fine. Each element's whole run - number, unit
// glyph, or the mode's own letters - typically sits inside one record's own
// contiguous chars, so scanning the raw bytes finds it even in a frame whose
// record chain desyncs elsewhere. The backward-scan-from-the-glyph logic is
// copied from scrape_locked()'s proven "number to the LEFT of a unit glyph"
// case, just walking the flat payload instead of one grid row.
void MspOsd::scan_osd_values(const uint8_t* payload, size_t size) {
    bool have_cell_v = false, have_pack_v = false, have_mode = false;
    float cell_v = 0.0f, pack_v = 0.0f;
    char mode[8] = {0};

    for (size_t k = 0; k < size; k++) {
        if (payload[k] != BF_SYM_VOLT) continue;
        long e = (long)k - 1;
        long b = e;
        while (b >= 0 && (isdigit(payload[b]) || payload[b] == '.')) b--;
        if (b >= 0 && payload[b] == '-') b--;
        b++;
        if (b > e || e < 0) continue;
        int n = (int)(e - b + 1);
        if (n <= 0 || n >= 16) continue;
        char num[16];
        memcpy(num, payload + b, n);
        num[n] = 0;
        float v = strtof(num, nullptr);
        // Same split as scrape_locked: one glyph serves both the pack reading
        // and the average-cell one, and 5V splits them.
        if (v > 0.0f && v < 5.0f)  { cell_v = v; have_cell_v = true; }
        else if (v >= 5.0f)        { pack_v = v; have_pack_v = true; }
    }

    for (size_t m = 0; m < sizeof(kBfModes) / sizeof(kBfModes[0]); m++) {
        size_t mlen = strlen(kBfModes[m]);
        if (mlen > size) continue;
        for (size_t k = 0; k + mlen <= size; k++) {
            if (memcmp(payload + k, kBfModes[m], mlen) != 0) continue;
            snprintf(mode, sizeof(mode), "%s", kBfModes[m]);
            for (int z = (int)strlen(mode) - 1; z >= 0 && mode[z] == ' '; z--) mode[z] = 0;
            have_mode = true;
            break;
        }
        if (have_mode) break;
    }

    if (have_cell_v || have_pack_v || have_mode) {
        std::lock_guard<std::mutex> lock(mtx);
        if (have_cell_v) { raw_have_cell_v_ = true; raw_cell_v_ = cell_v; }
        if (have_pack_v) { raw_have_pack_v_ = true; raw_pack_v_ = pack_v; }
        if (have_mode)   { raw_have_mode_ = true; snprintf(raw_mode_, sizeof(raw_mode_), "%s", mode); }
        raw_values_us_ = get_time_us();
    }
}

void MspOsd::scrape_locked() {
    BfTelem t;
    t.stamp_us = get_time_us();

    // One row at a time, as a plain char string: every value Betaflight draws
    // sits on a single row, and the units are single glyphs beside the digits.
    char line[GRID_W + 1];
    uint16_t raw[GRID_W];

    bool saw_disarmed_glyph = false;
    bool saw_disarmed_text  = false;

    for (int r = 0; r < GRID_H; r++) {
        for (int c = 0; c < GRID_W; c++) {
            raw[c]  = grid[r][c].char_idx;
            line[c] = (raw[c] >= 32 && raw[c] <= 126) ? (char)raw[c] : ' ';
        }
        line[GRID_W] = 0;

        for (int c = 0; c < GRID_W; c++) {
            uint16_t g = raw[c];

            if (g == BF_SYM_DISARMED) { saw_disarmed_glyph = true; }

            // Number to the LEFT of a unit glyph: "16.4<V>", "18.4<A>".
            if (g == BF_SYM_VOLT || g == BF_SYM_AMP_A || g == BF_SYM_AMP_B ||
                g == BF_SYM_ALT_M || g == BF_SYM_KMH  || g == BF_SYM_MPH) {
                int e = c - 1;
                int b = e;
                while (b >= 0 && (isdigit((unsigned char)line[b]) || line[b] == '.')) b--;
                if (b >= 0 && line[b] == '-') b--;          // negative altitude
                b++;
                if (b <= e && e >= 0) {
                    char num[16];
                    int n = e - b + 1;
                    if (n > 0 && n < (int)sizeof(num)) {
                        memcpy(num, line + b, n);
                        num[n] = 0;
                        float v = strtof(num, nullptr);
                        switch (g) {
                        case BF_SYM_VOLT:
                            // One glyph serves both the pack reading and the
                            // average-cell one. 5V splits them.
                            if (v > 0.0f && v < 5.0f)  { t.have_cell_v = true; t.cell_v = v; }
                            else if (v >= 5.0f)        { t.have_pack_v = true; t.pack_v = v; }
                            break;
                        case BF_SYM_AMP_A:
                        case BF_SYM_AMP_B:
                            t.have_amps = true; t.amps = v; break;
                        case BF_SYM_ALT_M:
                            t.have_alt = true;  t.alt_m = v; break;
                        case BF_SYM_KMH:
                            t.have_speed = true; t.speed_kmh = v; break;
                        case BF_SYM_MPH:
                            t.have_speed = true; t.speed_kmh = v * 1.609344f; break;
                        }
                    }
                }
            }

            // Pitch/Roll Angle: signed one-decimal number right after the
            // marker glyph, e.g. "<0x15>-01.4". strtof skips the format's own
            // leading space/sign for us.
            if (g == BF_SYM_PITCH || g == BF_SYM_ROLL) {
                if (c + 1 < GRID_W) {
                    float v = strtof(line + c + 1, nullptr);
                    if (g == BF_SYM_PITCH) { t.have_pitch = true; t.pitch_deg = v; }
                    else                   { t.have_roll  = true; t.roll_deg  = v; }
                }
            }

            // Number to the RIGHT of a marker glyph: "<rssi>99", "<lq>99",
            // "<flytime>03:21".
            bool is_timer = (g == BF_SYM_FLY_M || g == BF_SYM_ON_M);
            bool is_lq    = (g == BF_SYM_LQ || g == BF_SYM_LQ_ALT);
            if (g == BF_SYM_RSSI || is_lq || is_timer) {
                int b = c + 1;
                while (b < GRID_W && line[b] == ' ') b++;
                if (is_timer) {
                    // Read, for the arm state below, but never masked: the
                    // canopy runs its own flight timer off the arm transition,
                    // so whether Betaflight's timers show as well is the
                    // pilot's call, made in Betaflight's OSD setup.
                    int mm, ss;
                    if (b + 4 < GRID_W && sscanf(line + b, "%d:%d", &mm, &ss) == 2) {
                        t.have_timer = true;
                        t.timer_s = mm * 60 + ss;
                    }
                } else if (g == BF_SYM_RSSI) {
                    // Two different elements share this glyph: a percentage,
                    // and the CRSF dBm figure, which is negative. Reading the
                    // second as the first is why the RC track stayed empty -
                    // isdigit() rejected the minus sign outright, and the
                    // 0..100 range check would have thrown it away anyway.
                    //
                    // None of the three is masked: the canopy's RC track is
                    // Link Quality only, so Betaflight's RSSI stays on screen.
                    if (b < GRID_W && line[b] == '-') {
                        int v = atoi(line + b);
                        if (v < 0 && v > -200) { t.have_dbm = true; t.rssi_dbm = v; }
                    } else if (b < GRID_W && isdigit((unsigned char)line[b])) {
                        // A third element shares it too: TX uplink power,
                        // "<rssi>  10MW" - the RC link's power, which the
                        // canopy does not show (its power readout is the
                        // VTX's). A figure running straight into letters is
                        // that one, so it is neither read as RSSI nor masked.
                        int e = b;
                        while (e + 1 < GRID_W && isdigit((unsigned char)line[e + 1])) e++;
                        bool unit = e + 1 < GRID_W && isalpha((unsigned char)line[e + 1]);
                        int v = atoi(line + b);
                        if (!unit && v >= 0 && v <= 100) { t.have_rssi = true; t.rssi_pct = v; }
                    }
                } else if (b < GRID_W) {
                    // CRSF prints "<rf mode>:<lq>"; anything else is a bare
                    // percentage. Take the figure after the colon when there
                    // is one.
                    //
                    // Masked through the end of that figure, not to the first
                    // space: CRSF right-aligns the LQ in three places, so
                    // anything under 100 reads "{2:  0" and a mask that
                    // stopped at the padding left the digits on screen - seen
                    // live as a stray "0" under the canopy with no link up.
                    const char* colon = strchr(line + b, ':');
                    const char* num = (colon && colon - (line + b) <= 2) ? colon + 1 : line + b;
                    while (*num == ' ') num++;
                    if (isdigit((unsigned char)*num)) {
                        int v = atoi(num);
                        int e = (int)(num - line);
                        while (e + 1 < GRID_W && isdigit((unsigned char)line[e + 1])) e++;
                        if (v >= 0 && v <= 100) { t.have_lq = true; t.lq_pct = v; }
                    }
                }
            }
        }

        if (!t.have_mode) {
            for (size_t m = 0; m < sizeof(kBfModes) / sizeof(kBfModes[0]); m++) {
                const char* hit = strstr(line, kBfModes[m]);
                if (!hit) continue;
                t.have_mode = true;
                snprintf(t.mode, sizeof(t.mode), "%s", kBfModes[m]);
                // "HOR " and "AIR " carry their trailing space in the table so
                // they cannot match inside a longer word; the HUD does not
                // want to draw it.
                for (int k = (int)strlen(t.mode) - 1; k >= 0 && t.mode[k] == ' '; k--)
                    t.mode[k] = 0;
                break;
            }
        }
        if (strstr(line, "DISARMED")) saw_disarmed_text = true;
    }

    // Arm state. The flight timer is the only element that is unambiguous
    // about it - Betaflight only advances it while armed - so a timer that
    // moved in the last 3s means armed and one that has not means disarmed.
    // Without a timer element fall back to whatever says DISARMED outright,
    // and failing that leave it unknown so the HUD draws neither state.
    if (t.have_timer) {
        if (last_timer_s >= 0 && t.timer_s != last_timer_s) timer_moved_us = t.stamp_us;
        last_timer_s = t.timer_s;
        if (timer_moved_us != 0 && t.stamp_us - timer_moved_us < 3000000ULL) t.arm = 1;
        else if (saw_disarmed_glyph || saw_disarmed_text)                    t.arm = 0;
        else if (timer_moved_us != 0)                                        t.arm = 0;
    } else if (saw_disarmed_glyph || saw_disarmed_text) {
        t.arm = 0;
    }

    // osd_pitch_deg/osd_roll_deg (the reactive HUD's physics input) are NOT
    // set here - the grid this scrape reads can be missing a screen's worth
    // of records when handle_batched_displayport's validation rejects a
    // malformed frame (see its comment). scan_osd_angles() is the source of
    // truth for those, called on every 182 frame's raw bytes regardless of
    // whether the record walk above succeeded. t.have_pitch/pitch_deg here
    // stay purely informational (for a future on-screen readout).

    telem = t;
}

BfTelem MspOsd::get_telem() {
    std::lock_guard<std::mutex> lock(mtx);
    BfTelem t = telem;      // whatever the last screen's glyphs yielded
    uint64_t now = get_time_us();


    // The structured replies win wherever they exist: more precision, no
    // dependency on the pilot having enabled an OSD element, and for arm state
    // they are the only honest source. Both go stale after two seconds, so a
    // dropped link stops asserting a state rather than freezing on the last.
    if (msp_status_us_ && now - msp_status_us_ < 2000000ULL) {
        t.arm = msp_arm_;
        if (msp_arm_ == 1 && msp_armed_since_) {
            t.have_timer = true;
            t.timer_s = (int)((now - msp_armed_since_) / 1000000ULL);
        }
    }
    if (msp_analog_us_ && now - msp_analog_us_ < 2000000ULL) {
        if (msp_have_rssi_)   { t.have_rssi = true;   t.rssi_pct = msp_rssi_pct_; }
        if (msp_have_pack_v_) { t.have_pack_v = true; t.pack_v = msp_pack_v_; }
        if (msp_have_amps_)   { t.have_amps = true;   t.amps = msp_amps_; }
    }
    // scan_osd_values(): the grid-independent recovery of voltage/mode (see
    // its comment) - this FC's MSP_ANALOG never carries pack voltage
    // (msp_have_pack_v_ never sets), so this is the only reliable source for
    // it, and for mode always (mode has no structured MSP equivalent here).
    // Same 30s grace as osd_angles_us: the air only resends a row that
    // changed, so a held mode/voltage can go a while between repaints.
    // 5 minutes, not 30s: voltage/mode legitimately go long stretches with no
    // new value to resend (an idle, disarmed pack is dead flat), and 30s was
    // proven too short live - it read fine, then visibly vanished right at
    // the ~30s mark as raw_values_us_ aged out. A many-minutes-stale reading
    // is still far more useful than blanking a value that has not actually
    // changed.
    // "Fill in if missing" was wrong: t (from the grid scrape) can have
    // have_mode/have_pack_v/have_cell_v already TRUE from whatever the last
    // successful FULL grid parse happened to see - which, since that parse
    // only succeeds intermittently (handle_batched_displayport's comment),
    // can be considerably older than this raw scan. "Fill if missing" then
    // never overrides it, so a mode/voltage change sat on screen showing the
    // OLD value indefinitely once any grid parse had ever caught it -
    // looked exactly like "does not update right away". Prefer the raw scan
    // whenever it is actually newer than the grid's own capture, not just
    // when the grid has nothing at all.
    if (raw_values_us_ && now - raw_values_us_ < 300000000ULL) {
        bool raw_is_newer = raw_values_us_ > telem.stamp_us;
        if (raw_have_cell_v_ && (raw_is_newer || !t.have_cell_v)) { t.have_cell_v = true; t.cell_v = raw_cell_v_; }
        if (raw_have_pack_v_ && (raw_is_newer || !t.have_pack_v)) { t.have_pack_v = true; t.pack_v = raw_pack_v_; }
        if (raw_have_mode_   && (raw_is_newer || !t.have_mode))   { t.have_mode = true; snprintf(t.mode, sizeof(t.mode), "%s", raw_mode_); }
    }
    // The polled values win over the OSD text: exact, and there whether or not the
    // pilot enabled an OSD element. Each group goes stale after 2 s (GPS 4 s: it is
    // asked for less often), so a dead link stops asserting.
    {
        const MspTelemState& s = mt_.state();
        auto fresh = [&](unsigned g, uint64_t ms) {
            for (int i = 0; i < 6; i++)
                if (g == (1u << i)) return mt_us_[i] && now - mt_us_[i] < ms * 1000ULL;
            return false;
        };
        if (s.have_batt && fresh(MspTelem::G_BATT, 2000)) {
            if (s.pack_v > 0.5f) { t.have_pack_v = true; t.pack_v = s.pack_v; }
            if (s.cells > 0 && s.pack_v > 0.5f) { t.have_cell_v = true; t.cell_v = s.pack_v / s.cells; }
            t.have_amps = true; t.amps = s.amps;
            if (s.capacity_mah > 0) {
                int pct = 100 - (int)((100LL * s.mah_drawn) / s.capacity_mah);
                t.have_batt_pct = true; t.batt_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
            }
        }
        if (s.have_alt && fresh(MspTelem::G_ALT, 2000)) { t.have_alt = true; t.alt_m = s.alt_m; }
        if (s.have_gps && fresh(MspTelem::G_GPS, 4000)) {
            t.have_gps = true; t.gps_fix = s.fix; t.sats = s.sats;
            if (s.fix >= 1) {
                t.have_pos = true; t.lat = s.lat; t.lon = s.lon;
                t.have_speed = true; t.speed_kmh = s.gps_speed_kmh;
            }
        }
        if (s.have_home && fresh(MspTelem::G_HOME, 4000)) {
            t.have_home = true; t.home_dist_m = s.home_dist_m; t.home_dir_deg = s.home_dir_deg;
        }
        if (s.have_modes && fresh(MspTelem::G_MODES, 2000)) { t.have_mode = true; snprintf(t.mode, sizeof(t.mode), "%s", s.mode); }
    }
    for (int i = 0; i < 6; i++) if (mt_us_[i] > t.stamp_us) t.stamp_us = mt_us_[i];
    // t.stamp_us so far is only the last successful FULL grid parse
    // (scrape_locked, via handle_batched_displayport's all-or-nothing
    // validation - see its comment). Every consumer treats a stale stamp_us
    // as "no telemetry" and hides the whole panel (hud_canopy.cpp,
    // osd.cpp's strip HUD), so a grid validation failure was blanking
    // voltage/mode/etc even while the grid-independent scans above kept them
    // genuinely fresh underneath - looked exactly like "showed up, then
    // froze and disappeared". Bump stamp_us to whichever source is actually
    // most recent, so "is there live telemetry" reflects reality.
    if (msp_analog_us_  > t.stamp_us) t.stamp_us = msp_analog_us_;
    if (msp_status_us_  > t.stamp_us) t.stamp_us = msp_status_us_;
    if (raw_values_us_  > t.stamp_us) t.stamp_us = raw_values_us_;
    return t;
}

// MSP_STATUS / MSP_STATUS_EX. flightModeFlags is a bitmap over the active box
// IDs and BOXARM is always the first of them, so bit 0 is the arm state. That
// is the assumption every other MSP consumer makes, and it is the only source
// here that is not a side effect of what the pilot chose to put on their OSD.
void MspOsd::handle_status(const uint8_t* p, size_t n) {
    if (n < 11) return;
    uint32_t flags = (uint32_t)p[6] | ((uint32_t)p[7] << 8) |
                     ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
    int armed = (flags & 1u) ? 1 : 0;
    uint64_t now = get_time_us();
    // Our own flight timer, run off the arm transition rather than read back
    // out of Betaflight's timer element - so it exists whether or not the
    // pilot enabled that element, which this aircraft had not.
    if (armed && msp_arm_ != 1) msp_armed_since_ = now;
    // The camera's tilt: disarmed and still, note the camera's pitch against gravity; the moment it
    // goes from disarmed to armed the aircraft is level, so that is the tilt (and is saved).
    if (!armed) {
        double down[3];
        if (stab::ImuStream::get().resting_down(down)) {
            const double z = down[2] < -1.0 ? -1.0 : down[2] > 1.0 ? 1.0 : down[2];
            tilt_.rest(asin(-z) * 180.0 / 3.14159265358979323846, now);
        }
    } else if (msp_arm_ == 0) {
        if (tilt_.armed(now)) tilt_save_ = true;
    }
    if (armed != msp_arm_) content_ver_++;
    msp_arm_ = armed;
    msp_status_us_ = now;
}

// MSP_ANALOG. The legacy layout is 7 bytes; Betaflight appends a centivolt
// pack voltage, which is the one worth having when it is there.
void MspOsd::handle_analog(const uint8_t* p, size_t n) {
    if (n < 7) return;
    uint16_t rssi   = (uint16_t)(p[3] | (p[4] << 8));          // 0..1023
    int16_t  amps   = (int16_t)(p[5] | (p[6] << 8));           // 0.01 A
    float    pack_v = p[0] / 10.0f;                            // 0.1 V, legacy
    if (n >= 9) {
        uint16_t v16 = (uint16_t)(p[7] | (p[8] << 8));
        if (v16 > 0) pack_v = v16 / 100.0f;
    }
    if (rssi <= 1023) {
        msp_have_rssi_ = true;
        msp_rssi_pct_  = (int)((rssi * 100 + 511) / 1023);
    }
    // 2.5V floor, not 0.5: some FCs (seen live) leave the legacy byte
    // unpopulated when they only fill the extended v16 field, and a stray
    // small value there (0.1V steps, so anything under ~2.5) reads as
    // "0.7V pack" - implausible for any connected flight battery (even one
    // dead Li cell sits above that) - and, being ">0.5", used to win over
    // scan_osd_values()'s actually-correct DisplayPort-scraped reading below
    // instead of leaving the field for it.
    // The HUD shows both to a tenth; a change it would not show is not news.
    if ((pack_v > 2.5f && (int)(pack_v * 10) != (int)(msp_pack_v_ * 10)) ||
        amps / 10 != (int)(msp_amps_ * 10))
        content_ver_++;
    if (pack_v > 2.5f) { msp_have_pack_v_ = true; msp_pack_v_ = pack_v; }
    msp_have_amps_ = true;
    msp_amps_ = amps / 100.0f;
    msp_analog_us_ = get_time_us();
}

uint32_t MspOsd::content_version() {
    // The camera's IMU is the reactive HUD's input: turning or tilting at all is news.
    if (motion_wanted_) {
        double w[3];
        if (stab::ImuStream::get().recent_rate_dps(20000, w) && (fabs(w[0]) > 3.0 || fabs(w[1]) > 3.0 || fabs(w[2]) > 3.0))
            content_ver_++;
    }
    return content_ver_.load(std::memory_order_relaxed);
}

bool MspOsd::take_cam_tilt_to_save(int* deg) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!tilt_save_) return false;
    tilt_save_ = false;
    *deg = (int)lround(tilt_.deg());
    return true;
}

MspOsd::MspCounts MspOsd::msp_counts() {
    std::lock_guard<std::mutex> lock(mtx);
    MspCounts c;
    c.frames = n_msp_;
    c.bytes = n_msp_bytes_;
    c.active = msp_frame_us_ && get_time_us() - msp_frame_us_ < 3000000ULL;
    return c;
}

// A polled response has just updated mt_. mtx is held.
void MspOsd::apply_polled_locked(unsigned groups) {
    const uint64_t now = get_time_us();
    const MspTelemState& s = mt_.state();
    for (int i = 0; i < 6; i++) if (groups & (1u << i)) mt_us_[i] = now;
    if (groups & (MspTelem::G_BATT | MspTelem::G_MODES)) content_ver_++;
    static uint64_t logged_us = 0;
    if (now - logged_us >= 5000000ULL) {      // what the flight controller says, every 5 s
        logged_us = now;
        const stab::ImuStream::Status is = stab::ImuStream::get().status();
        double wl[3] = {0, 0, 0};
        stab::ImuStream::get().recent_rate_dps(20000, wl);
        printf("imu: %s %.0f samples/s, %llu messages, camera rate pitch %.1f pan %.1f roll %.1f deg/s, tilt %.1f deg, gyro offset %.2f %.2f %.2f deg/s\n",
               is.live ? "live" : "NOT live", is.rate_hz, (unsigned long long)is.messages, wl[0], wl[1], wl[2], tilt_.deg(),
               is.bias_dps[0], is.bias_dps[1], is.bias_dps[2]);
        double wf[3] = {0, 0, 0};
        if (fc_rate_locked(wf))
            printf("fc imu: rate pitch %.0f yaw %.0f roll %.0f deg/s (raw roll %d pitch %d yaw %d), acc %d %d %d\n",
                   wf[0], wf[1], wf[2], fc_gyro_[0], fc_gyro_[1], fc_gyro_[2], raw_acc_x, raw_acc_y, raw_acc_z);
        printf("msp: batt %s%.2fV %s%.2fA %dmAh cells=%d | alt=%s%.1fm | gps=%s fix=%d sats=%d | home=%s%dm@%d | mode=%s armed=%d\n",
               s.have_batt ? "" : "-", s.pack_v, s.have_batt ? "" : "-", s.amps, s.mah_drawn, s.cells,
               s.have_alt ? "" : "-", s.alt_m, s.have_gps ? "yes" : "no", s.fix, s.sats,
               s.have_home ? "" : "-", s.home_dist_m, s.home_dir_deg, s.have_modes ? s.mode : "-", s.have_arm ? (int)s.armed : -1);
    }
}

void MspOsd::handle_msp_frame(uint16_t function, const uint8_t* payload, size_t size) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        msp_frame_us_ = get_time_us();
        n_msp_++;
        n_msp_bytes_ += (uint32_t)size + (is_v2 ? 9 : 6);   // the frame: header and checksum too
        const unsigned groups = mt_.handle(function, payload, size);
        if (groups) apply_polled_locked(groups);
    }
    if (function == 101 || function == 150) {   // MSP_STATUS / _EX
        std::lock_guard<std::mutex> lock(mtx);
        handle_status(payload, size);
        return;
    }
    if (function == 110) {                      // MSP_ANALOG
        std::lock_guard<std::mutex> lock(mtx);
        handle_analog(payload, size);
        return;
    }
    if (function == 182) {
        scan_osd_angles(payload, size);
        scan_osd_values(payload, size);
    }
    if (function == 182 && handle_batched_displayport(payload, size)) return;
    if (function == 182) { // MSP_DISPLAYPORT
        if (size < 1) return;
        uint8_t subcmd = payload[0];

        std::lock_guard<std::mutex> lock(mtx);

        if (subcmd == 0) { // MSP_DP_HEARTBEAT
             // Do nothing (keepalive)
        } else if (subcmd == MSP_DP_CLEAR_SCREEN) {
            // Clears pending, not grid - grid is what the renderer reads and
            // only changes on DRAW, so a CLEAR that never arrives costs
            // nothing (DRAW just commits pending with whatever WRITEs landed
            // on top of its last content, same as if CLEAR had been skipped)
            // and a CLEAR that does arrive actually clears, which used to be
            // a no-op here entirely - any cell a screen's WRITEs did not
            // happen to touch stayed on screen forever, through DRAW after
            // DRAW, because nothing ever blanked it. That is what a still
            // image several sessions deep looked like: every session's
            // leftovers, never removed, only ever partly painted over.
            clear_internal();
        } else if (subcmd == MSP_DP_WRITE_STRING) {
            if (size >= 4) {
                uint8_t row = payload[1];
                uint8_t col = payload[2];
                // uint8_t attr = payload[3]; // Not used yet

                // The enclosing branch already holds mtx (see the lock_guard
                // above), so take the non-locking variant - write_string()
                // would deadlock on this non-recursive mutex.
                write_string_locked(row, col, payload + 4, size - 4);
            }
        } else if (subcmd == MSP_DP_DRAW_SCREEN) {
            // COMMIT pending to active - and tell the OSD only if it differs
            if (memcmp(grid, pending_grid, sizeof(grid)) != 0) {
                memcpy(grid, pending_grid, sizeof(grid));
                content_ver_++;
            }
            // DON'T clear pending - let WRITE commands overwrite naturally
            // This makes us immune to CLEAR packet loss
            scrape_locked();
        }
    } else if (function == 102) { // MSP_RAW_IMU
        if (size >= 6) {
            std::lock_guard<std::mutex> lock(mtx);
            int16_t ax = (int16_t)(payload[0] | (payload[1] << 8));
            int16_t ay = (int16_t)(payload[2] | (payload[3] << 8));
            int16_t az = (int16_t)(payload[4] | (payload[5] << 8));
            if (motion_wanted_ && (ax != raw_acc_x || ay != raw_acc_y || az != raw_acc_z))
                content_ver_++;
            raw_acc_x = ax;
            raw_acc_y = ay;
            raw_acc_z = az;
            if (size >= 12) {
                for (int i = 0; i < 3; i++)
                    fc_gyro_[i] = (int16_t)(payload[6 + 2 * i] | (payload[7 + 2 * i] << 8));
                fc_gyro_us_ = get_time_us();
                // Turning at all is news for the reactive HUD, as with the camera's IMU.
                if (motion_wanted_ && (abs(fc_gyro_[0]) > 3 || abs(fc_gyro_[1]) > 3 || abs(fc_gyro_[2]) > 3))
                    content_ver_++;
            }
        }
    }
}

void MspOsd::write_string(uint8_t row, uint8_t col, const uint8_t* str, size_t len) {
    std::lock_guard<std::mutex> lock(mtx);
    write_string_locked(row, col, str, len);
}

void MspOsd::write_string_locked(uint8_t row, uint8_t col, const uint8_t* str, size_t len) {
    // Basic bounds check
    if (row >= GRID_H) return;

    for (size_t i = 0; i < len; i++) {
        if (col + i >= GRID_W) break; // Clip line

        // Write to PENDING grid
        // Betaflight/INAV typically sends ASCII.
        pending_grid[row][col + i].char_idx = str[i];
    }
}

void MspOsd::draw(math::Mat4& projection, math::Mat4& view, std::function<void(float x, float y, const std::vector<uint16_t>& span)> draw_span_cb) {
    std::lock_guard<std::mutex> lock(mtx);
    
    // Grid to Screen Mapping: Simple Centered Layout
    float base_cell_w = 0.03375f; 
    float base_cell_h = 0.060f;
    
    float cell_w = base_cell_w * m_scale;
    float cell_h = base_cell_h * m_scale;
    
    float start_x = -( (float)GRID_W * cell_w ) / 2.0f;
    float start_y = ( (float)GRID_H * cell_h ) / 2.0f;
    
    // Adjust start_x to center the character in the cell
    start_x += cell_w / 2.0f; 

    for(int y=0; y<GRID_H; y++) {
        for(int x=0; x<GRID_W; x++) {
            uint16_t ch = grid[y][x].char_idx;
            if (ch != 0 && ch != ' ') {
                // Determine span start
                std::vector<uint16_t> span;
                float px = start_x + ( (float)x * cell_w );
                float py = start_y - ( (float)y * cell_h );
                
                // Collect contiguous non-space characters
                while (x < GRID_W) {
                    uint16_t current_ch = grid[y][x].char_idx;
                    if (current_ch == 0 || current_ch == ' ') break;
                    span.push_back(current_ch);
                    x++;
                }
                
                if (!span.empty()) {
                    draw_span_cb(px, py, span);
                }
                
                // Inner loop increments x, so decrement to account for outer loop's x++
                x--;
            }
        }
    }
}

void MspOsd::draw_region(int r_start, int c_start, int r_cnt, int c_cnt, float x, float y, std::function<void(float x, float y, const std::vector<uint16_t>& span)> draw_span_cb) {
    std::lock_guard<std::mutex> lock(mtx);

    const float cell_w = m_cell_w;
    const float cell_h = m_cell_h;
    auto blank = [&](int r, int c) {
        uint16_t ch = grid[r][c].char_idx;
        return ch == 0 || ch == ' ';
    };
    
    for(int r = r_start; r < r_start + r_cnt && r < GRID_H; r++) {
        for(int c = c_start; c < c_start + c_cnt && c < GRID_W; c++) {
            if (!blank(r, c)) {
                std::vector<uint16_t> span;
                float px = x + (float)(c - c_start) * cell_w;
                float py = y - (float)(r - r_start) * cell_h;
                
                // Text: a single space between two text characters stays inside the run, so a
                // phrase ("LOW BATTERY") is laid out as one string. An icon, or two or more
                // spaces, ends it.
                auto is_text = [&](uint16_t ch) { return ch >= 0x21 && ch <= 0x5F && ch != '$'; };
                while (c < c_start + c_cnt && c < GRID_W) {
                    uint16_t current_ch = grid[r][c].char_idx;
                    if (blank(r, c)) {
                        if (current_ch == ' ' && !span.empty() && is_text(span.back()) &&
                            c + 1 < c_start + c_cnt && c + 1 < GRID_W &&
                            is_text(grid[r][c + 1].char_idx)) {
                            span.push_back(' ');
                            c++;
                            continue;
                        }
                        break;
                    }
                    span.push_back(current_ch);
                    c++;
                }
                
                if (!span.empty()) {
                    draw_span_cb(px, py, span);
                }
                c--;
            }
        }
    }
}

void MspOsd::simulate_startup() {
    //return; 
    auto hex_to_bytes = [](const std::string& hex) {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < hex.length(); i += 2) {
            std::string byteString = hex.substr(i, 2);
            uint8_t byte = (uint8_t) strtol(byteString.c_str(), NULL, 16);
            bytes.push_back(byte);
        }
        return bytes;
    };

    std::vector<std::string> cmds = {
        "244D3E09B6030C00004F53445F3186244D3E01B604B3",
        "0C00004F53445F3186244D3E01B604B3",
        "8314244D3E05B6030621008611244D3E05B6030622008014244D3E05B6030523008214244D3E0AB6031306009331342E39",
        "4D3E0AB6030A0000437A2032390EA9244D3E0AB603132F00012020304D57A8244D3E0AB6030E0000524154455F31DD244D3E09B6030A00005049445F3182244D3E09B6030C00004F53445F3186244D3E01B604B3",
        "091D008327244D3E05B603081E008620244D3E05B603081F008027244D3E05B6030720008314244D3E05B6030621008611244D3E05B6030622008014244D3E05B6030523008214244D3E0A",
        "B6031306009331342E39062D244D3E08B603090A0041495220C4244D3E0BB603130C002020322E33399A2D244D3E09B6031313002020313007BA244D3E09B6031323007F302E300CD1",
        "244D3E0FB60312000142415454203C2046554C4C85244D3E0AB60313000093332E37320621244D",
        "3E0AB6030A0000437A2032390EA9244D3E0AB603132F00012020304D57A8244D3E0AB6030E0000524154455F31DD244D3E09B6030A00005049445F3182244D3E09B6030C00004F53445F3186244D3E01B604B3",
        "081F008027244D3E05B6030720008314244D3E05B6030621008611244D3E05B6030622008014244D3E05B60305230082",
        "14244D3E0AB6031306009331342E39062D244D3E08B603090A0041495220C4244D3E0BB603130C002020322E33309A24244D3E09B6031313002020313007BA244D3E09B6031323007F302E300CD124",
    };

    for (const auto& s : cmds) {
        auto bytes = hex_to_bytes(s);
        parse_bytes(bytes.data(), bytes.size());
    }
}

// The flight controller's gyro (MSP_RAW_IMU, polled ~20 a second) as the reactive HUD's rates:
// x pitch (+ nose up), y yaw (+ turning right), z roll (+ right side down), deg/s. Betaflight's
// axes are already the aircraft's (after its board alignment); only the signs differ. Stale after
// 200 ms: a few polls missed.
bool MspOsd::fc_rate_locked(double w[3]) const {
    if (!fc_gyro_us_ || get_time_us() - fc_gyro_us_ > 200000ULL) return false;
    w[0] = -fc_gyro_[1];
    w[1] = -fc_gyro_[2];
    w[2] = fc_gyro_[0];
    return true;
}

void MspOsd::update_physics(float dt_sec) {
    if (dt_sec <= 0.0f) return;
    if (dt_sec > 0.1f) dt_sec = 0.1f; // Clamp to avoid dynamic instability on lags

    // The HUD answers MOVEMENT, not attitude: the camera's angular rates (the air unit's IMU,
    // 1 kHz) push it, and the spring brings it back to centre when nothing moves - held at a
    // steady tilt it sits at rest. Without the IMU it stays centred.
    // The camera's rates, put into the AIRCRAFT's axes by the camera's tilt on the airframe: a yaw seen
    // by a camera tilted up is partly a turn about its lens, and would bank the HUD. x: pitch rate
    // (+ nose up), y: yaw (+ turning right), z: roll rate (+ right side down), deg/s.
    double wc[3] = {0, 0, 0}, w[3];
    bool imu = stab::ImuStream::get().recent_rate_dps(20000, wc);
    stab::to_aircraft_rates(wc, tilt_.deg(), w);
    // No camera IMU sending (the Lite+ has none, fc-imu): the flight controller's gyro, already in
    // the aircraft's axes, at its 20 a second.
    bool fc = false;
    if (!imu) imu = fc = fc_rate_locked(w);
    // The flight controller's rates are single readings 20 a second, not the camera IMU's 20 ms
    // average at 1 kHz: at the same gain the HUD moved too much and too jerkily. Half, at every level.
    const float kFcImuScale = 0.5f;
    const float src_scale = fc ? kFcImuScale : 1.0f;
    // A rate to an offset: against the motion (the HUD lags behind it), nothing under 3 deg/s (the
    // gyro's noise and what is left of its offset), full at 360 deg/s, never past the maximum.
    auto slide = [](double rate_dps, float max) {
        if (fabs(rate_dps) < 3.0) return 0.0f;
        float v = -(float)(rate_dps / 360.0) * max;
        return v > max ? max : v < -max ? -max : v;
    };
    // 0.12 is what a turn felt right at; the SMALL/MEDIUM/EXTREME setting scales it.
    const float kMaxOffset = 0.12f * reactivity_scale_ * src_scale;
    // Pitch moves the HUD up and down, yaw sideways. Roll does not slide it: a banking aircraft
    // tilts the HUD, below.
    const float target_y = imu ? slide(w[0], kMaxOffset) : 0.0f;
    const float target_x = imu ? slide(w[1], kMaxOffset) : 0.0f;

    // Bank: a view-plane rotation from the roll rate, scaled well below 1:1 - a full roll would
    // swing the panels through 90 deg and read as the HUD falling over, where the point is to
    // suggest the bank.
    const float kDegToRad   = 3.14159265f / 180.0f;
    const float kMaxBankRad = 3.5f * kDegToRad * reactivity_scale_ * src_scale;   // never tilt further than this
    const float target_bank = imu ? slide(w[2], kMaxBankRad) : 0.0f;

    // Mass-spring-damper constants
    float stiffness = 220.0f; // Spring constant (omega^2)
    float damping = 16.0f;    // Damping coefficient (2 * zeta * omega)

    // At rest, a target within about a pixel is not worth moving to: sensor
    // jitter on a still craft would otherwise start a settling animation
    // (60 Hz for half a second) over and over for motion nobody can see.
    const float kDeadPos = 0.0025f, kDeadBank = 0.0015f;   // ~1 px at 1080p
    if (!hud_moving_ &&
        fabsf(target_x - hud_pos_x) < kDeadPos && fabsf(target_y - hud_pos_y) < kDeadPos &&
        fabsf(target_bank - hud_pos_bank) < kDeadBank)
        return;

    // Fixed substeps of at most 10 ms. The OSD now draws only as often as it
    // has to (10 Hz with nothing moving), and semi-implicit Euler with these
    // constants goes unstable past ~80 ms steps - one integration per frame
    // would fling the HUD on the first frame after a quiet spell.
    int steps = (int)(dt_sec / 0.010f) + 1;
    float h = dt_sec / steps;
    for (int i = 0; i < steps; i++) {
        float acc_x = -stiffness * (hud_pos_x - target_x) - damping * hud_vel_x;
        float acc_y = -stiffness * (hud_pos_y - target_y) - damping * hud_vel_y;
        float acc_b = -stiffness * (hud_pos_bank - target_bank) - damping * hud_vel_bank;
        hud_vel_x += acc_x * h;
        hud_vel_y += acc_y * h;
        hud_vel_bank += acc_b * h;
        hud_pos_x += hud_vel_x * h;
        hud_pos_y += hud_vel_y * h;
        hud_pos_bank += hud_vel_bank * h;
    }

    // At rest when it is within a hair of the target and barely moving:
    // snap there, so the OSD can stop asking for frames. The thresholds are
    // well under a pixel (the offsets are in frustum units, ~1.3 per half
    // screen height).
    const float kPosEps = 0.0005f, kVelEps = 0.005f, kBankEps = 0.0002f;
    hud_moving_ = !(fabsf(hud_pos_x - target_x) < kPosEps && fabsf(hud_vel_x) < kVelEps &&
                    fabsf(hud_pos_y - target_y) < kPosEps && fabsf(hud_vel_y) < kVelEps &&
                    fabsf(hud_pos_bank - target_bank) < kBankEps && fabsf(hud_vel_bank) < kVelEps);
    if (!hud_moving_) {
        hud_pos_x = target_x; hud_pos_y = target_y; hud_pos_bank = target_bank;
        hud_vel_x = hud_vel_y = hud_vel_bank = 0.0f;
    }
}

void MspOsd::get_hud_offset(float& dx, float& dy, float& bank_rad, float scale, bool& moving) {
    std::lock_guard<std::mutex> lock(mtx);
    reactivity_scale_ = scale;
    uint64_t now_us = get_time_us();
    if (last_physics_update_us == 0) {
        last_physics_update_us = now_us;
    }
    float dt = (float)(now_us - last_physics_update_us) / 1000000.0f;
    last_physics_update_us = now_us;
    // At rest, the time since the last frame was spent at rest: integrating
    // it against a new target would jump the HUD most of the way in one
    // frame (frames come 100 ms apart when nothing moves). Start from one
    // frame's worth instead.
    if (!hud_moving_ && dt > 1.0f / 60.0f) dt = 1.0f / 60.0f;

    update_physics(dt);

    dx = hud_pos_x;
    dy = hud_pos_y;
    bank_rad = hud_pos_bank;
    moving = hud_moving_;

}

void MspOsd::reset_screen() {
    std::lock_guard<std::mutex> lock(mtx);
    // Both buffers: pending_grid is what write_string_locked paints into and
    // grid is what the renderer reads, and a frame may not arrive to carry a
    // cleared pending across.
    memset(grid, 0, sizeof(grid));
    clear_internal();
    telem = BfTelem();

    // Everything scraped off that screen, and everything the structured MSP
    // replies left behind - all of it described the aircraft that just went
    // away.
    raw_have_cell_v_ = raw_have_pack_v_ = raw_have_mode_ = false;
    raw_cell_v_ = raw_pack_v_ = 0.0f;
    raw_mode_[0] = 0;
    raw_values_us_ = 0;
    osd_pitch_deg = osd_roll_deg = 0.0f;
    osd_angles_us = 0;
    raw_acc_x = raw_acc_y = 0; raw_acc_z = 2048;

    mt_.reset();
    for (auto& u : mt_us_) u = 0;

    msp_arm_ = -1;
    msp_armed_since_ = 0;
    msp_status_us_   = 0;
    msp_have_rssi_   = msp_have_pack_v_ = msp_have_amps_ = false;
    msp_rssi_pct_    = 0;
    msp_pack_v_      = msp_amps_ = 0.0f;
    msp_analog_us_   = 0;
    last_timer_s     = -1;
    timer_moved_us   = 0;

    hud_pos_x = hud_pos_y = hud_pos_bank = 0.0f;
    hud_vel_x = hud_vel_y = hud_vel_bank = 0.0f;
    last_physics_update_us = 0;

    content_ver_++;
}

void MspOsd::reset_hud_motion() {
    std::lock_guard<std::mutex> lock(mtx);
    hud_pos_x = hud_pos_y = hud_pos_bank = 0.0f;
    hud_vel_x = hud_vel_y = hud_vel_bank = 0.0f;
    // So the next live frame integrates from "now" rather than charging the
    // spring with however long the goggle sat idle.
    last_physics_update_us = 0;
}


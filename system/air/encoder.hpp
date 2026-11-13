#pragma once
// The air unit's video encoder: the cap on its bitrate, and the one on its
// camera's exposure (kestrel-air's two caps on the picture it sends), and
// the strips it sends each picture in.
#include <cstdint>

#include "system/part.hpp"

namespace air {

struct EncoderState {
    int max_kbps = 0;              // 0: none
    int max_exp_us = 0;            // 0: none
    int strips = 1;                // pictures in strips (1: whole pictures)
    uint32_t taken = 0;            // the last change carried out (Ticket::seq)
};

// Max Bitrate: kestrel-air's video bitrate cap, kbps (0 = AUTO: it follows the
// link, up to 20 Mbps; a cap above that lets kestrel-air go to it, 40 at most,
// on a link that carries it - 40 MHz). Below the link's own rate it buys a
// steadier picture and shorter slices on the air; stock air units ignore it.
static const int kArMaxBrVals[]     = { 0, 6000, 8000, 10000, 12000, 15000, 18000, 20000, 22000, 25000,
                                        30000, 35000, 40000 };
static const char* kArMaxBrLabels[] = { "AUTO", "6 Mbps", "8 Mbps", "10 Mbps", "12 Mbps",
                                        "15 Mbps", "18 Mbps", "20 Mbps", "22 Mbps", "25 Mbps",
                                        "30 Mbps", "35 Mbps", "40 Mbps" };
static const int kArMaxBrCount = 13;
static inline int ar_maxbr_index(int kbps) {
    for (int i = 0; i < kArMaxBrCount; i++) if (kArMaxBrVals[i] == kbps) return i;
    return 0;
}
// Max Exposure: the longest exposure the air unit's auto exposure may use, us
// (0, OFF: its camera's own - 10 ms indoors on the P1, 16.5 in the dark). Past
// it the gain makes up the rest: less blur and a fresher picture, more grain
// in low light. A kestrel-air that says maxexp=1 in its info takes it live.
static const int kArMaxExpVals[]     = { 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 0 };
static const char* kArMaxExpLabels[] = { "1 ms", "2 ms", "3 ms", "4 ms", "5 ms", "6 ms", "8 ms",
                                         "10 ms", "OFF" };
static const int kArMaxExpCount = 9;
static inline int ar_maxexp_index(int us) {
    for (int i = 0; i < kArMaxExpCount; i++) if (kArMaxExpVals[i] == us) return i;
    return 3;   // 4 ms
}
// Strips: each picture encoded and sent as strips of rows, the top one out of
// the air unit's encoder sooner (ar8030_strips; OFF: whole pictures).
static const int kArStripsVals[]     = { 1, 2, 3, 4 };
static const char* kArStripsLabels[] = { "OFF", "2", "3", "4" };
static const int kArStripsCount = 4;
// The air unit's feature bit (air::UnitState::features) saying both ends can:
// its kestrel-air sends strips and this goggle joins them (= the radio's
// Ar8030Link::kAirFeatStrips).
static const uint16_t kFeatStrips = 0x2000;
static inline int ar_strips_index(int n) {
    for (int i = 0; i < kArStripsCount; i++) if (kArStripsVals[i] == n) return i;
    return n < 1 ? 0 : kArStripsCount - 1;
}

class Encoder : public Part<EncoderState> {
public:
    virtual Ticket set_max_bitrate(int kbps) = 0;   // RADIO > Max Bitrate (kestrel-air takes it)
    virtual Ticket set_max_exposure(int us) = 0;    // VIDEO > Max Exposure (a kestrel-air that says maxexp=1)
    virtual Ticket set_strips(int n) = 0;           // VIDEO > Strips (one that says strips=N/M, M > 1)
};

}  // namespace air

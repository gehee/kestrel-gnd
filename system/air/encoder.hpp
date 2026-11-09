#pragma once
// The air unit's video encoder: the cap on its bitrate.
#include <cstdint>

#include "system/part.hpp"

namespace air {

struct EncoderState {
    int max_kbps = 0;              // 0: none
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

class Encoder : public Part<EncoderState> {
public:
    virtual Ticket set_max_bitrate(int kbps) = 0;   // RADIO > Max Bitrate (kestrel-air takes it)
};

}  // namespace air

#pragma once
// The air unit's video encoder: the cap on its bitrate.
#include <cstdint>

#include "system/part.hpp"

namespace air {

struct EncoderState {
    int max_kbps = 0;              // 0: none
    uint32_t taken = 0;            // the last change carried out (Ticket::seq)
};

class Encoder : public Part<EncoderState> {
public:
    virtual Ticket set_max_bitrate(int kbps) = 0;   // RADIO > Max Bitrate (kestrel-air takes it)
};

}  // namespace air

#pragma once
// The air unit's radio: the power it sends at, the levels it offers.
#include <cstdint>

#include "system/part.hpp"

namespace air {

struct RadioState {
    int tx_power_mw = 0;           // TX Power as it runs (stock's encoding, system/air/power.hpp)
    int tx_power_want_mw = 0;      // TX Power as chosen, fitted to each air unit
    uint32_t levels_n = 0;         // its power levels are known: TX Power's row
    int prj = 0;                   // its power-level set, its project number (0: not known yet)
    uint32_t taken = 0;            // the last change carried out (Ticket::seq)
};

class Radio : public Part<RadioState> {
public:
    virtual Ticket set_tx_power(int mw) = 0;      // stock's encoding: N, or N+1 for AUTO capped at N
};

}  // namespace air

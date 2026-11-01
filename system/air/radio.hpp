#pragma once
// The air unit's radio: the power it sends at, the levels it offers, the
// blocks it sent again.
#include <cstdint>

#include "system/part.hpp"

namespace air {

struct RadioState {
    int tx_power_mw = 0;           // TX Power as it runs (stock's encoding, system/air/power.hpp)
    int tx_power_want_mw = 0;      // TX Power as chosen, fitted to each air unit
    int live_dbm = 0;              // as it last announced it (Open8030 event 14), dBm; 0: not known
    uint32_t levels_n = 0;         // its power levels are known: TX Power's row
    int prj = 0;                   // its power-level set, its project number (0: not known yet)
    bool resend_known = false;     // its resends are known (take_resend)
    uint32_t taken = 0;            // the last change carried out (Ticket::seq)
};

// A radio frame in which it sent blocks again (Open8030 at both ends): when
// it ended (CLOCK_MONOTONIC us), and v: the blocks (1..2), or 3: none
// confirmed (the broadcast stalled). The stats screen's.
struct Resend { uint64_t t_us; uint8_t v; };

class Radio : public Part<RadioState> {
public:
    virtual Ticket set_tx_power(int mw) = 0;      // stock's encoding: N, or N+1 for AUTO capped at N
    // Its resends, every one, oldest first (they wake no one); false: none left.
    virtual bool take_resend(Resend& r) = 0;
    virtual uint64_t resends_dropped() const = 0;   // that came faster than they were taken
};

}  // namespace air

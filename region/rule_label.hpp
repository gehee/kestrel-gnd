#pragma once
// A channel's verdict (zones::judge) and its power cap, worded as both channel
// pages write them - the VRX Pro's OSD and the native Ascent's menu - so the
// two say the same thing about the same channel.
#include <cstdio>
#include <string>

#include "region/zones.hpp"

namespace zones {

// A power cap: "1 W", "25 mW".
inline void cap_text(int mw, char* out, size_t n) {
    if (mw >= 1000 && mw % 1000 == 0) std::snprintf(out, n, "%d W", mw / 1000);
    else std::snprintf(out, n, "%d mW", mw);
}

// NOT ALLOWED, INDOOR ONLY, LICENCE NEEDED, OK - AMATEUR, OK - <cap> MAX or OK;
// RULES UNKNOWN for a country with none (judge's kNoRules - the VRX Pro's page
// says that once, over the whole chart, and never asks here). sep stands
// between OK and what follows it (the VRX Pro's pixel font spaces it out).
inline std::string rule_label(const Verdict& v, const char* sep = " - ") {
    switch (v.state) {
    case kNotAllowed: return "NOT ALLOWED";
    case kIndoor:     return "INDOOR ONLY";
    case kLicence:    return "LICENCE NEEDED";
    case kNoRules:    return "RULES UNKNOWN";
    default: break;
    }
    if (v.amateur) return std::string("OK") + sep + "AMATEUR";
    if (v.cap_mw > 0) {
        char cap[16];
        cap_text(v.cap_mw, cap, sizeof(cap));
        return std::string("OK") + sep + cap + " MAX";
    }
    return "OK";
}

}  // namespace zones

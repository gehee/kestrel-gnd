#pragma once

// SYSTEM > Time Zone: one setting for where you fly. It is a named zone
// ("America/Toronto"), which says two things a POSIX TZ string alone cannot:
// the time the clock shows, and the country - whose rules decide which of the
// radio's channels the channel page offers.
//
// The zones are generated from tzdata (tools/gen_zones.py -> zones_data.cpp),
// with each country's ITU region. The rules come in two layers:
//   - licence-exempt: our own table, kept by hand (fpv_rules.cpp), each line
//     with the regulator's text it comes from. It holds only for certified
//     equipment, and the page says so.
//   - amateur: the 5 cm amateur band of the country's ITU region (Radio
//     Regulations, Article 5) - for every country, but only with an amateur
//     licence (SYSTEM > Amateur Licence). National rules can narrow it.
// Not wireless-regdb: that only describes the Wi-Fi bands, and the AR8030's
// channels reach below them, into 4.x GHz - which neither layer opens.
//
// The zone in force is saved as "time_zone". Picking one applies its TZ string
// to this process (the clock, recording names) on the thread that calls
// apply_pending_tz() - the OSD's - since setenv() is not safe to race with
// the time functions other threads call.

#include <cstdint>
#include <string>
#include <vector>

namespace zones {

struct Country { const char* code; const char* name; int itu_region; };   // 0: none (Antarctica)
struct Zone    { const char* name; const char* city; int area; int country; const char* posix; };
struct Alias   { const char* name; int zone; };
// One range a country opens to an FPV video link (fpv_rules.cpp).
struct FpvRule {
    const char* country;     // ISO 3166 code, as in zone.tab
    int         lo_mhz, hi_mhz;
    int         cap_mw;      // 0: no cap known
    bool        indoor;      // indoors only
    const char* note;        // where it comes from, shown on the page
};

// The generated tables (zones_data.cpp). Declared here so its definitions
// have external linkage: a const at namespace scope is file-local otherwise.
namespace data {
extern const char* const kTzdataVersion;
extern const char* const kAreas[];
extern const int kAreaCount;
extern const Country kCountries[];
extern const int kCountryCount;
extern const Zone kZones[];
extern const int kZoneCount;
extern const Alias kAliases[];
extern const int kAliasCount;
extern const FpvRule kFpvRules[];   // fpv_rules.cpp
extern const int kFpvRuleCount;
}  // namespace data

int            zone_count();
const Zone&    zone(int i);
int            area_count();
const char*    area_name(int a);
int            country_count();
const Country& country(int c);
const char*    tzdata_version();
// Whether anything is known for country c: a licence-exempt line, or an ITU region.
bool           has_rules(int c);

// A zone by name, old names included (Asia/Calcutta finds Asia/Kolkata). -1: none.
int find(const std::string& name);

// The menu's lists, in the order it walks them: the countries with a zone in
// an area (by name), and a country's zones in that area (by city). Zones are
// stored in that order, so a country's zones in an area are one run.
std::vector<int> countries_in(int area);
std::vector<int> zones_in(int area, int country);

// ---- what a country allows ----
enum Legal { kAllowed = 0, kIndoor = 1, kLicence = 2, kNotAllowed = 3, kNoRules = 4 };
struct Verdict {
    Legal       state = kNoRules;
    bool        amateur = false;   // allowed through the amateur band (with the licence)
    int         cap_mw = 0;        // the lowest power cap of the ranges it sits in (0: none known)
    int         rule_lo = 0, rule_hi = 0;   // the range that decided it, MHz
    const char* note = "";         // where the deciding rule comes from
};
// A channel centred on centre_mhz, bw_mhz wide, in country c, for a pilot
// with or without an amateur licence. Judged by its whole width: inside the
// licence-exempt lines is allowed (indoor only if a range it touches is);
// otherwise inside those and the amateur band together needs the licence;
// otherwise not allowed. kNoRules when nothing is known for the country.
Verdict judge(int c, int centre_mhz, int bw_mhz, bool licensed);

// The amateur band of an ITU region (1-3), MHz; false for none.
bool amateur_band(int region, int& lo_mhz, int& hi_mhz);

// SYSTEM > Amateur Licence, saved as "amateur_licence".
bool licensed();
void set_licensed(bool on);

// ---- the setting ----
int  selected();                 // the zone in force, -1: not set
int  selected_country();         // its country, -1: not set
void load();                     // read "time_zone" at start and apply it
void select(int z);              // pick a zone (-1: none), save it, have its TZ applied
uint32_t generation();           // bumped by every select(), for anyone mirroring it
// Apply the zone's TZ string to this process if it changed. Call from one
// thread only (the OSD's). True when it did.
bool apply_pending_tz();

}  // namespace zones

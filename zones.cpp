#include "zones.hpp"
#include "settings.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>

namespace zones {

namespace {
std::atomic<int>      g_sel{-1};
std::atomic<uint32_t> g_gen{0};
std::atomic<bool>     g_tz_pending{false};
std::string           g_tz_initial;        // the TZ the init script exported (time.conf)
bool                  g_tz_had_initial = false;
}

int            zone_count()     { return data::kZoneCount; }
const Zone&    zone(int i)      { return data::kZones[std::max(0, std::min(i, data::kZoneCount - 1))]; }
int            area_count()     { return data::kAreaCount; }
const char*    area_name(int a) { return (a >= 0 && a < data::kAreaCount) ? data::kAreas[a] : "--"; }
int            country_count()  { return data::kCountryCount; }
const Country& country(int c)   { return data::kCountries[std::max(0, std::min(c, data::kCountryCount - 1))]; }
const char*    tzdata_version() { return data::kTzdataVersion; }

int find(const std::string& name) {
    if (name.empty()) return -1;
    for (int i = 0; i < data::kZoneCount; i++)
        if (name == data::kZones[i].name) return i;
    for (int i = 0; i < data::kAliasCount; i++)
        if (name == data::kAliases[i].name) return data::kAliases[i].zone;
    return -1;
}

std::vector<int> countries_in(int area) {
    std::vector<int> v;
    for (int i = 0; i < data::kZoneCount; i++) {
        const Zone& z = data::kZones[i];
        if (z.area == area && (v.empty() || v.back() != z.country)) v.push_back(z.country);
    }
    return v;
}

std::vector<int> zones_in(int area, int c) {
    std::vector<int> v;
    for (int i = 0; i < data::kZoneCount; i++)
        if (data::kZones[i].area == area && data::kZones[i].country == c) v.push_back(i);
    return v;
}

namespace {

// The 5 cm amateur band per ITU region (Radio Regulations, Article 5: a
// secondary allocation): 5650-5850 MHz in Regions 1 and 3, to 5925 in Region 2.
const int kAmateurLo[4] = { 0, 5650, 5650, 5650 };
const int kAmateurHi[4] = { 0, 5850, 5925, 5850 };
const char* const kAmateurNote[4] = {
    "",
    "Amateur band, ITU Region 1 (Radio Regulations art. 5): with an amateur licence; national rules may narrow it",
    "Amateur band, ITU Region 2 (Radio Regulations art. 5): with an amateur licence; national rules may narrow it",
    "Amateur band, ITU Region 3 (Radio Regulations art. 5): with an amateur licence; national rules may narrow it",
};

std::atomic<bool> g_licensed{false};

struct Range { int lo2, hi2, cap_mw; bool indoor; const char* note; };

// The country's licence-exempt lines, and with `amateur` its region's amateur band, in half-MHz.
int ranges_of(int c, bool amateur, Range* out, int cap) {
    int n = 0;
    const char* code = data::kCountries[c].code;
    for (int i = 0; i < data::kFpvRuleCount && n < cap; i++) {
        const FpvRule& r = data::kFpvRules[i];
        if (!strcmp(r.country, code))
            out[n++] = { r.lo_mhz * 2, r.hi_mhz * 2, r.cap_mw, r.indoor, r.note ? r.note : "" };
    }
    const int reg = data::kCountries[c].itu_region;
    if (amateur && reg >= 1 && reg <= 3 && n < cap)
        out[n++] = { kAmateurLo[reg] * 2, kAmateurHi[reg] * 2, 0, false, kAmateurNote[reg] };
    return n;
}

// Whether ranges r[0..n) cover [lo, hi] without a gap; their lowest known cap.
bool covers(const Range* r, int n, int lo, int hi, int& cap) {
    int x = lo;
    cap = 0;
    for (;;) {
        const Range* next = nullptr;
        for (int i = 0; i < n; i++)
            if (r[i].lo2 <= x && r[i].hi2 > x && (!next || r[i].hi2 > next->hi2)) next = &r[i];
        if (!next) return false;
        if (next->cap_mw > 0) cap = cap ? std::min(cap, next->cap_mw) : next->cap_mw;
        x = next->hi2;
        if (x >= hi) return true;
    }
}

// The range holding the centre, for the note; else the first one touched.
const Range* deciding(const Range* r, int n, int lo, int hi) {
    const int mid = (lo + hi) / 2;
    const Range* touched = nullptr;
    for (int i = 0; i < n; i++) {
        if (r[i].hi2 <= lo || r[i].lo2 >= hi) continue;
        if (r[i].lo2 <= mid && r[i].hi2 > mid) return &r[i];
        if (!touched) touched = &r[i];
    }
    return touched;
}

}  // namespace

bool has_rules(int c) {
    if (c < 0 || c >= data::kCountryCount) return false;
    if (data::kCountries[c].itu_region > 0) return true;
    for (int i = 0; i < data::kFpvRuleCount; i++)
        if (!strcmp(data::kFpvRules[i].country, data::kCountries[c].code)) return true;
    return false;
}

bool amateur_band(int region, int& lo_mhz, int& hi_mhz) {
    if (region < 1 || region > 3) return false;
    lo_mhz = kAmateurLo[region];
    hi_mhz = kAmateurHi[region];
    return true;
}

bool licensed() { return g_licensed.load(); }

void set_licensed(bool on) {
    if (on == g_licensed.load()) return;
    g_licensed = on;
    Settings::getInstance().set("amateur_licence", on);
}

Verdict judge(int c, int centre_mhz, int bw_mhz, bool licensed) {
    Verdict v;
    if (!has_rules(c)) return v;                   // kNoRules
    if (bw_mhz < 1) bw_mhz = 1;
    // In half-MHz, so a 5 MHz-wide channel's edges stay whole.
    const int lo = centre_mhz * 2 - bw_mhz, hi = centre_mhz * 2 + bw_mhz;
    Range r[32];
    int cap = 0;

    // Licence-exempt first.
    const int ne = ranges_of(c, false, r, 32);
    if (ne > 0 && covers(r, ne, lo, hi, cap)) {
        v.state = kAllowed;
        v.cap_mw = cap;
        const Range* d = deciding(r, ne, lo, hi);
        for (int i = 0; i < ne; i++)
            if (r[i].indoor && r[i].hi2 > lo && r[i].lo2 < hi) { v.state = kIndoor; d = &r[i]; break; }
        if (d) { v.note = d->note; v.rule_lo = d->lo2 / 2; v.rule_hi = d->hi2 / 2; }
        return v;
    }
    // Then with the amateur band: the licence decides.
    const int na = ranges_of(c, true, r, 32);
    if (na > ne && covers(r, na, lo, hi, cap)) {
        const Range& am = r[na - 1];
        v.state = licensed ? kAllowed : kLicence;
        v.amateur = true;
        v.note = am.note;
        v.rule_lo = am.lo2 / 2; v.rule_hi = am.hi2 / 2;
        return v;
    }
    v.state = kNotAllowed;
    return v;
}

int selected() { return g_sel.load(); }

int selected_country() {
    const int z = g_sel.load();
    return z >= 0 ? data::kZones[z].country : -1;
}

uint32_t generation() { return g_gen.load(); }

void load() {
    g_licensed = Settings::getInstance().getBool("amateur_licence", false);
    if (const char* t = getenv("TZ")) { g_tz_initial = t; g_tz_had_initial = true; }
    const int z = find(Settings::getInstance().getString("time_zone", ""));
    g_sel = z;
    g_gen++;
    // Not set: the TZ the init script exported (time.conf) stays in force.
    if (z >= 0) {
        setenv("TZ", data::kZones[z].posix, 1);
        tzset();
        printf("time zone: %s (%s, %s)\n", data::kZones[z].name,
               data::kCountries[data::kZones[z].country].code, data::kZones[z].posix);
    }
}

void select(int z) {
    if (z < -1 || z >= data::kZoneCount) return;
    if (z == g_sel.load()) return;
    g_sel = z;
    g_gen++;
    Settings::getInstance().set("time_zone", std::string(z >= 0 ? data::kZones[z].name : ""));
    g_tz_pending = true;
}

bool apply_pending_tz() {
    if (!g_tz_pending.exchange(false)) return false;
    const int z = g_sel.load();
    if (z >= 0) {
        setenv("TZ", data::kZones[z].posix, 1);
        printf("time zone: %s (%s)\n", data::kZones[z].name, data::kZones[z].posix);
    } else {
        // Not set again: back to what the init script exported.
        if (g_tz_had_initial) setenv("TZ", g_tz_initial.c_str(), 1);
        else unsetenv("TZ");
        printf("time zone: not set (TZ from time.conf)\n");
    }
    tzset();
    fflush(stdout);
    return true;
}

}  // namespace zones

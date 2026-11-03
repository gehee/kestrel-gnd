#pragma once
// The radios' TX power levels and what each air unit is offered, and the
// air unit's names by its project number.

#include <cmath>

// TX power levels, in mW, with the dBm the radio actually wants.
//
// Recovered from ar_ldy_gnd's do_bb_set_local_power (table at 0x30b250, chosen
// when the --subtype global reads 2, i.e. this board - the same guard as the
// PRJ_DISPATCH call). Stock's encoding is reused verbatim: a value of N mW
// means "hold N", and N+1 means "auto, capped at N" - which is where its menu
// strings "500mW Auto" and "1W Auto" come from.
//
// Which of them an air unit is offered is stock's choice, made per air unit:
// GlassesUI's menu builder (0x70f30 in the 18.21.10 GlassesUI) picks one of
// six {label, value} lists at 0x6f55d0 by the project number the air unit
// reports in its camera settings (TLV 0x11, sky_prj_name):
//
//   4 Ascent Lite, 6 Ascent RC   25mW, 100mW, 100mW Auto       (also unknown)
//   7 Ascent Lite+               25mW, 100mW, 200mW, 500mW, 500mW Auto
//   5, 8 the GT family           up to 2W .. 10W by RF board - not offered here
//
// (A goggle setting can narrow every unit to 25mW or 25/100mW; not modelled.)
//
// Ours: a kestrel-air that says it takes them (KA_FEAT_PWRLVL) is offered 150,
// 200 and 316 mW (24 dBm) besides, each with its AUTO (N + 1: 151, 201, 317),
// on a Lite+: AUTO's top can be set under 500 mW. The menu shows it as two
// rows, the mode (FIXED / AUTO) and the level (the fixed level, or AUTO's
// top); the value on the wire stays stock's.
// Until the air unit has reported, stock's global is 0, which gets the Lite's
// list: the smaller one is the default here too.
struct ar_pwr_level { int mw; int dbm; bool automode; const char* label; };
// The mw field doubles as the value sent to the air unit (cmd 0x22), where
// stock's convention is that AUTO is the level plus one - a bbproxy capture of
// stock caught 101 on the wire for its "100mW Auto" entry, so 501 here means
// "auto, capped at 500 mW". This table holds every level of both lists; what a
// menu offers is kArPwrOffer*.
// dbm is the VRX's own radio level for it (stock's table: 3 dB under the
// label, kept - the board's gain after the chip is not known). Nothing above
// 500 mW: the 630 / 790 mW TEST levels (27 / 28 dBm) are gone - at 27 dBm the
// Lite+'s SNR was 1.5-4.7 dB under 200 mW's at every distance with a stronger
// signal (flight 2026-10-30): its amplifier distorts past ~26.
static const ar_pwr_level kArPwrLevels[] = {
    {   25, 11, false, "25mW"      }, {  100, 17, false, "100mW"     },
    {  101, 17, true,  "100mW AUTO"},
    {  150, 19, false, "150mW"     }, {  151, 19, true,  "150mW AUTO"},
    {  200, 20, false, "200mW"     }, {  201, 20, true,  "200mW AUTO"},
    {  316, 22, false, "316mW"     }, {  317, 22, true,  "316mW AUTO"},
    {  500, 24, false, "500mW"     }, {  501, 24, true,  "500mW AUTO"},
};
static const int kArPwrCount = (int)(sizeof(kArPwrLevels) / sizeof(kArPwrLevels[0]));

// An air unit's TX power until it is given one (tx_power_mw, and each air
// unit's own sky_<MAC>_tx_power_mw): the lowest level, held. Stock starts at
// 500mW AUTO; the goggle's own uplink follows the same setting.
static const int kArPwrDefaultMw = 25;

static inline int ar_pwr_index(int mw) {
    for (int i = 0; i < kArPwrCount; i++) if (kArPwrLevels[i].mw == mw) return i;
    for (int i = 0; i < kArPwrCount; i++) if (kArPwrLevels[i].mw == kArPwrDefaultMw) return i;
    return 0;   // a value no level matches: the default
}

// What an air unit is offered, by the project number it reports (0: not yet).
enum { AR_AIR_LITE = 4, AR_AIR_RC = 6, AR_AIR_LITE_PLUS = 7 };
static const int kArPwrOfferLite[]     = { 25, 100, 101 };
static const int kArPwrOfferLitePlus[] = { 25, 100, 200, 500, 501 };
// With kestrel-air's own levels (fine): the fixed ones, then their AUTO.
static const int kArPwrOfferLitePlusFine[] = { 25, 100, 150, 200, 316, 500, 151, 201, 317, 501 };
struct ar_pwr_offer { const int *mw; int n; };
static inline ar_pwr_offer ar_pwr_offered(int air_prj, bool fine = false) {
    if (air_prj == AR_AIR_LITE_PLUS) {
        if (fine) return { kArPwrOfferLitePlusFine, 10 };
        return { kArPwrOfferLitePlus, 5 };
    }
    return { kArPwrOfferLite, 3 };
}
static inline const char *ar_air_name(int air_prj) {
    switch (air_prj) {
        case AR_AIR_LITE: return "Ascent Lite";
        case AR_AIR_RC: return "Ascent RC";
        case AR_AIR_LITE_PLUS: return "Ascent Lite+";
        default: return "unknown";
    }
}
// The serial number's prefix stock's Device Info gives an air unit of this
// project number (GlassesUI: "%s%06X" with the chip ID's low 24 bits).
static inline const char *ar_air_sn_prefix(int air_prj) {
    switch (air_prj) {
        case 0: return "Avatar_";
        case 1: return "AvatarSE_";
        case 2: return "AvatarMini_";
        case 3: return "L_Sky_";
        case 4: case 6: case 7: return "H_Sky_";
        case 5: return "G_Sky_";
        case 8: return "GR_Sky_";
        default: return "AvatarX_";
    }
}
// mw's place in the air unit's list, or -1.
static inline int ar_pwr_offer_index(int air_prj, int mw, bool fine = false) {
    ar_pwr_offer o = ar_pwr_offered(air_prj, fine);
    for (int i = 0; i < o.n; i++) if (o.mw[i] == mw) return i;
    return -1;
}
// A level the air unit is offered for mw: mw itself, or else the highest one
// at or under it - AUTO stays AUTO where the list has it (500mW AUTO -> 100mW
// AUTO on a Lite).
static inline int ar_pwr_fit(int air_prj, int mw, bool fine = false) {
    if (ar_pwr_offer_index(air_prj, mw, fine) >= 0) return mw;
    ar_pwr_offer o = ar_pwr_offered(air_prj, fine);
    const ar_pwr_level &want = kArPwrLevels[ar_pwr_index(mw)];
    const int cap = want.automode ? want.mw - 1 : want.mw;
    int fixed = o.mw[0], autolv = 0;
    for (int i = 0; i < o.n; i++) {
        const ar_pwr_level &c = kArPwrLevels[ar_pwr_index(o.mw[i])];
        if (c.automode) { if (c.mw - 1 <= cap) autolv = c.mw; }
        else if (c.mw <= cap) fixed = c.mw;
    }
    return want.automode && autolv ? autolv : fixed;
}

// The menu's two rows over the one value: the mode, and the level (the fixed
// level, or AUTO's top, in mW).
static inline bool ar_pwr_is_auto(int mw) { return kArPwrLevels[ar_pwr_index(mw)].automode; }
static inline int ar_pwr_top(int mw) {
    const ar_pwr_level &l = kArPwrLevels[ar_pwr_index(mw)];
    return l.automode ? l.mw - 1 : l.mw;
}
// The levels the air unit has in a mode, lowest first: what the level row steps
// through. Stock's lists have AUTO at one level only (100 on a Lite, 500 on a
// Lite+).
static const int kArPwrTopsMax = 8;
static inline int ar_pwr_tops(int air_prj, bool fine, bool automode, int out[kArPwrTopsMax]) {
    ar_pwr_offer o = ar_pwr_offered(air_prj, fine);
    int n = 0;
    for (int i = 0; i < o.n && n < kArPwrTopsMax; i++)
        if (kArPwrLevels[ar_pwr_index(o.mw[i])].automode == automode) out[n++] = ar_pwr_top(o.mw[i]);
    return n;
}
// The value for a mode at a level; where the mode has not that level, the next
// one up (else the highest it has). -1: the air unit has no such mode.
static inline int ar_pwr_pick(int air_prj, bool fine, bool automode, int top) {
    int t[kArPwrTopsMax];
    const int n = ar_pwr_tops(air_prj, fine, automode, t);
    if (n == 0) return -1;
    int pick = t[n - 1];
    for (int i = 0; i < n; i++) if (t[i] >= top) { pick = t[i]; break; }
    return automode ? pick + 1 : pick;
}

// The radio reports its current power in dBm; convert to mW for display.
//
// An exact table row wins, so the anchors read as the round numbers the menu
// uses. Everything else is computed - stock's table is log-linear on
// 25 mW @ 11 dBm and reproduces every anchor to within a milliwatt
// (17 -> 99.5, 20 -> 198.6, 24 -> 498.8), so the curve between them is the
// table's own.
//
// This matters for AUTO: the range handed to the radio is 11..24 dBm, and the
// levels it actually sits at in between (12-16, 18, 19, 21-23) have no table
// row. Matching anchors only would mean falling back to dBm exactly when AUTO
// is doing its job.
static inline int ar_pwr_mw_from_dbm(int dbm) {
    for (int i = 0; i < kArPwrCount; i++)
        if (!kArPwrLevels[i].automode && kArPwrLevels[i].dbm == dbm)
            return kArPwrLevels[i].mw;
    if (dbm <= 0) return 0;
    return (int)(25.0 * pow(10.0, (dbm - 11) / 10.0) + 0.5);
}

// The same for the air unit, on its stock scale: its radio level is 1 dB
// under the label (kestrel-air: 25/100/150/200/500 mW = 13/19/21/22/26), so a
// level reads as the menu's figure for it.
static inline int ar_air_mw_from_dbm(int dbm) {
    static const int lv[][2] = { {13, 25}, {19, 100}, {21, 150}, {22, 200}, {24, 316}, {26, 500} };
    for (const auto &l : lv) if (l[0] == dbm) return l[1];
    if (dbm <= 0) return 0;
    return (int)(pow(10.0, (dbm + 1) / 10.0) + 0.5);
}
// And back: the air unit's radio level for a level in mW (what AUTO's top is
// asked in, an Open8030 radio's SET 9 window).
static inline int ar_air_dbm_from_mw(int mw) {
    static const int lv[][2] = { {13, 25}, {19, 100}, {21, 150}, {22, 200}, {24, 316}, {26, 500} };
    for (const auto &l : lv) if (l[1] == mw) return l[0];
    if (mw <= 0) return 0;
    return (int)(10.0 * log10((double)mw) + 0.5) - 1;
}

// bb_phy_mcs_e -> the MCS number Artosyn names it by. The radio reports the
// enum's index, and the enum starts with two repeated-BPSK levels below MCS 0
// (BB_PHY_MCS_NEG_2, _NEG_1), so index 12 is MCS 10 (64QAM 2/3): shown as such,
// the rates line up (7.83 Mbps per coded bit per carrier at 20 MHz). Values
// are kept as the index everywhere else; only what is printed is renamed.
static inline int ar_mcs_label(int idx) { return idx - 2; }

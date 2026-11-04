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
//   5, 8 the GT family           25 mW through 5W AUTO. The goggle uplink
//                                stays at or below 27 dBm (stock SET 9 max
//                                0x1B). cmd 0x22 carries the mW; G_Sky 17.5.15
//                                map_pwr_dbm_486 turns that into the air dBm
//                                (1W 27, 2W 30, 4W 33, 5W 34). 700 and 3000 mW
//                                exist on the air and are not menu rows.
//
// (A goggle setting can narrow every unit to 25mW or 25/100mW; not modelled.)
// Until the air unit has reported, stock's global is 0, which gets the Lite's
// list: the smaller one is the default here too.
struct ar_pwr_level { int mw; int dbm; bool automode; const char* label; };
// The mw field doubles as the value sent to the air unit (cmd 0x22), where
// stock's convention is that AUTO is the level plus one - a bbproxy capture of
// stock caught 101 on the wire for its "100mW Auto" entry, so 501 here means
// "auto, capped at 500 mW" and 5001 means "auto, capped at 5 W". This table
// holds every level of every list; what a menu offers is kArPwrOffer*.
// dbm is the goggle uplink only. From 1W up it stays 27: the air unit, not
// the goggle, is what map_pwr_dbm_486 turns into 30/33/34 dBm. A readback of
// 27 dBm therefore matches the 1W row; the menu shows the commanded mW.
static const ar_pwr_level kArPwrLevels[] = {
    {   25, 11, false, "25mW"      }, {  100, 17, false, "100mW"     },
    {  101, 17, true,  "100mW AUTO"},
    {  200, 20, false, "200mW"     }, {  500, 24, false, "500mW"     },
    {  501, 24, true,  "500mW AUTO"},
    { 1000, 27, false, "1W"        }, { 1001, 27, true,  "1W AUTO"   },
    { 2000, 27, false, "2W"        }, { 2001, 27, true,  "2W AUTO"   },
    { 4000, 27, false, "4W"        }, { 4001, 27, true,  "4W AUTO"   },
    { 5000, 27, false, "5W"        }, { 5001, 27, true,  "5W AUTO"   },
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
static const int kArPwrOfferGt[] = {
    25, 100, 101, 200, 500, 501, 1000, 1001, 2000, 2001, 4000, 4001, 5000, 5001
};
struct ar_pwr_offer { const int *mw; int n; };
static inline ar_pwr_offer ar_pwr_offered(int air_prj) {
    if (air_prj == 5 || air_prj == 8) return { kArPwrOfferGt, 14 };
    if (air_prj == AR_AIR_LITE_PLUS) return { kArPwrOfferLitePlus, 5 };
    return { kArPwrOfferLite, 3 };
}
static inline const char *ar_air_name(int air_prj) {
    switch (air_prj) {
        case AR_AIR_LITE: return "Ascent Lite";
        case AR_AIR_RC: return "Ascent RC";
        case AR_AIR_LITE_PLUS: return "Ascent Lite+";
        case 5: return "Ascent GT";
        case 8: return "Ascent GT Pro";
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
static inline int ar_pwr_offer_index(int air_prj, int mw) {
    ar_pwr_offer o = ar_pwr_offered(air_prj);
    for (int i = 0; i < o.n; i++) if (o.mw[i] == mw) return i;
    return -1;
}
// A level the air unit is offered for mw: mw itself, or else the highest one
// at or under it - AUTO stays AUTO where the list has it (500mW AUTO -> 100mW
// AUTO on a Lite).
static inline int ar_pwr_fit(int air_prj, int mw) {
    if (ar_pwr_offer_index(air_prj, mw) >= 0) return mw;
    ar_pwr_offer o = ar_pwr_offered(air_prj);
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

// bb_phy_mcs_e -> the MCS number Artosyn names it by. The radio reports the
// enum's index, and the enum starts with two repeated-BPSK levels below MCS 0
// (BB_PHY_MCS_NEG_2, _NEG_1), so index 12 is MCS 10 (64QAM 2/3): shown as such,
// the rates line up (7.83 Mbps per coded bit per carrier at 20 MHz). Values
// are kept as the index everywhere else; only what is printed is renamed.
static inline int ar_mcs_label(int idx) { return idx - 2; }

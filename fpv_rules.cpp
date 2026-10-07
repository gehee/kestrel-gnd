// The licence-exempt layer of the channel page's rules (zones.hpp): ranges a
// country opens to a video link without any licence. Kept by hand.
//
// One line per range, under the country's ISO 3166 code (as zones_data.cpp
// has it), each citing the regulator's text it comes from - `note` is shown
// on the page. These hold for CERTIFIED equipment only: an air unit that is
// not certified for the country gets nothing from them, and the page says so.
// Caps are what the text states; conducted and e.i.r.p. are not the same.
//
// The amateur layer needs no lines here: every country gets its ITU region's
// 5 cm amateur band from zones.cpp, for pilots with an amateur licence.
//
// Not from wireless-regdb (Wi-Fi bands only), and not from memory: add a
// country only with its regulator's text in hand.

#include "zones.hpp"

namespace zones {
namespace data {

#define EU_SRD(cc) { cc, 5725, 5875, 25, false, \
    "EU SRD Decision 2006/771/EC as amended by (EU) 2022/180, band 61: 25 mW e.i.r.p., certified equipment" }
#define EEA_SRD(cc) { cc, 5725, 5875, 25, false, \
    "EEA: EU SRD Decision 2022/180 (EEA Joint Committee 254/2022), band 61: 25 mW e.i.r.p., certified equipment" }

const FpvRule kFpvRules[] = {
    // European Union: Commission Implementing Decision (EU) 2022/180, annex
    // band 61 - non-specific short-range devices, 5725-5875 MHz, 25 mW e.i.r.p.
    EU_SRD("AT"), EU_SRD("BE"), EU_SRD("BG"), EU_SRD("HR"), EU_SRD("CY"), EU_SRD("CZ"),
    EU_SRD("DK"), EU_SRD("EE"), EU_SRD("FI"), EU_SRD("FR"), EU_SRD("DE"), EU_SRD("GR"),
    EU_SRD("HU"), EU_SRD("IE"), EU_SRD("IT"), EU_SRD("LV"), EU_SRD("LT"), EU_SRD("LU"),
    EU_SRD("MT"), EU_SRD("NL"), EU_SRD("PL"), EU_SRD("PT"), EU_SRD("RO"), EU_SRD("SK"),
    EU_SRD("SI"), EU_SRD("ES"), EU_SRD("SE"),
    // EEA states outside the EU, which take the SRD Decision in.
    EEA_SRD("IS"), EEA_SRD("LI"), EEA_SRD("NO"),

    // United Kingdom: Ofcom IR 2030, non-specific short range devices.
    { "GB", 5725, 5875, 25, false, "Ofcom IR 2030: 25 mW e.i.r.p., certified equipment" },

    // United States: 47 CFR 15.247, digital modulation systems.
    { "US", 5725, 5850, 1000, false, "FCC 47 CFR 15.247: 1 W conducted, digital modulation, certified equipment" },

    // Canada: ISED RSS-247 Issue 4 (July 2025).
    { "CA", 5725, 5850, 1000, false, "ISED RSS-247 Issue 4: 1 W conducted, certified equipment" },
    { "CA", 5150, 5250, 200, true, "ISED RSS-247 Issue 4: indoors only, 200 mW e.i.r.p., certified equipment" },

    // Australia: Radiocommunications (Low Interference Potential Devices) Class Licence 2015.
    { "AU", 5725, 5850, 4000, false, "ACMA LIPD Class Licence 2015: 4 W e.i.r.p., digital modulation, certified equipment" },
};
const int kFpvRuleCount = (int)(sizeof(kFpvRules) / sizeof(kFpvRules[0]));

#undef EU_SRD
#undef EEA_SRD

}  // namespace data
}  // namespace zones

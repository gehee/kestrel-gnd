// Host test of radio/ar8030_sky's video modes as an air unit lists them (its
// info's modes=, kestrel-air on the P1 Air): the list's modes it names, by the
// size and rate SET_CHN_RES asks for; what is asked for in place of one it
// does not run; and which of the list a goggle's Video Mode row offers
// (system/air/unit.hpp's video_mode_offered), listed or not.
//   cmake -DKESTREL_TESTS=ON ... && ctest -R sky_modes_test
#include <cstdio>
#include <cstring>

#include "radio/ar8030_sky.hpp"
#include "system/air/unit.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int index_of(const char* name) {
    for (int i = 0; i < sky::kFpvModeCount; i++)
        if (!std::strcmp(sky::kFpvModes[i].name, name)) return i;
    return -1;
}
static uint32_t bit(const char* name) { return 1u << index_of(name); }

int main() {
    // The P1 Air's: 1080p60 and 720p90, or 1080p60 alone where its sensor
    // driver has no 720p modes; a list of the stock four and 864p120.
    CHECK(sky::modes_of("1920x1080@60,1280x720@90") == (bit("1080p60") | bit("720p90")));
    CHECK(sky::modes_of("1920x1080@60") == bit("1080p60"));
    CHECK(sky::modes_of("1280x720@60,1280x720@120,1920x1080@60,1920x1080@120,1536x864@120") ==
          (bit("720p60") | bit("720p100") | bit("1080p60") | bit("1080p100") | bit("864p120")));
    // By the rate asked for on the wire: 720p100 is 1280x720@120, not @100.
    CHECK(sky::modes_of("1280x720@100") == 0);
    // What the list has not, and what is not a mode, are left out; what
    // follows a rate is skipped.
    CHECK(sky::modes_of("2560x1440@30,junk,,1920x1080@60") == bit("1080p60"));
    CHECK(sky::modes_of("1920x1080@60:60") == bit("1080p60"));
    CHECK(sky::modes_of("") == 0);
    // 720p90 is asked for at 90, on the wire as in the list.
    const int m90 = index_of("720p90");
    CHECK(m90 >= 0 && sky::kFpvModes[m90].w == 1280 && sky::kFpvModes[m90].h == 720 &&
          sky::kFpvModes[m90].fps == 90 && sky::kFpvModes[m90].need);

    // In place of a mode it does not run: the fastest it runs of that size,
    // else the default (1080p100) where it runs it, else the last it runs.
    const uint32_t p1 = sky::modes_of("1920x1080@60,1280x720@90");
    CHECK(sky::fallback_mode_index(p1, 1280, 720) == m90);               // its 720p100 or 720p60
    CHECK(sky::fallback_mode_index(p1, 1920, 1080) == index_of("1080p60")); // its 1080p100
    CHECK(sky::fallback_mode_index(p1, 1536, 864) == index_of("1080p60"));  // its 864p
    CHECK(sky::fallback_mode_index(bit("1080p100") | bit("720p60"), 1536, 864) == sky::default_fpv_mode_index());
    CHECK(sky::fallback_mode_index(bit("1080p60"), 1280, 720) == index_of("1080p60"));
    CHECK(sky::fallback_mode_index(0, 1920, 1080) == -1);

    // The Video Mode row: what the list says where there is one, whatever
    // the feature bits; the modes the features give where there is none.
    const int m864 = index_of("864p120"), m1080 = index_of("1080p60"), m100 = index_of("1080p100");
    const int m720 = index_of("720p100");
    const uint16_t need864 = sky::kFpvModes[m864].need, need90 = sky::kFpvModes[m90].need;
    CHECK(air::video_mode_offered(true, bit("1080p60"), 0, m1080, 0));
    CHECK(!air::video_mode_offered(true, bit("1080p60"), 0, m100, 0));
    CHECK(!air::video_mode_offered(true, bit("1080p60"), need864, m864, need864));
    CHECK(air::video_mode_offered(true, p1, 0, m90, need90));
    CHECK(!air::video_mode_offered(true, p1, need90, m720, 0));
    CHECK(air::video_mode_offered(false, 0, 0, m100, 0));
    CHECK(!air::video_mode_offered(false, 0, 0, m864, need864));
    CHECK(air::video_mode_offered(false, 0, need864, m864, need864));
    CHECK(!air::video_mode_offered(false, 0, 0, m90, need90));   // stock, the Lites
    CHECK(air::video_mode_offered(false, 0, need90, m90, need90));

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}

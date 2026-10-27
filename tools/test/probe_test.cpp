// Unit test of hal::pick(): which board a device tree's compatible list names,
// in this build. On the unit it also prints what hal::board() picked there.
//   cmake -DKESTREL_TESTS=ON ... && ctest     (or run the binary on the unit)
#include <cstdio>
#include <string>

#include "hal/probe.hpp"
#include "kestrel_gnd_features.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// The build options' own rules, as the features header states them.
static_assert(KESTREL_HAL_ROCKCHIP + KESTREL_HAL_AR9311 + KESTREL_HAL_PC == 1,
              "exactly one HAL backend set");
static_assert(!KESTREL_STREAM_DECODE || KESTREL_HAL_ROCKCHIP, "stream decode is Rockchip's");

static std::string name(const bsp::Board &b) { return b.name; }

int main() {
    // A VRX Pro's tree as /proc/device-tree/compatible reads, NULs as spaces.
    const std::string vrx_pro = "rockchip,rk3568-evb1-ddr4-v10 rockchip,rk3568 ";
    const std::string rk3566  = "rockchip,rk3566 ";
    const std::string ar9311  = "artosyn,proxima-9311 ";

#if KESTREL_HAL_ROCKCHIP
    CHECK(name(hal::pick(vrx_pro)) == "vrx_pro");
    CHECK(name(hal::pick(rk3566)) == "vrx_pro");
#else
    // Without the Rockchip backends a Rockchip tree is no box we know.
    CHECK(name(hal::pick(vrx_pro)) == "pc");
#endif
    CHECK(name(hal::pick(ar9311)) == "pc");     // no AR9311 boards yet
    CHECK(name(hal::pick("")) == "pc");          // no device tree: a PC
    CHECK(name(hal::pick("rockchip,rk3588 ")) == "pc");

    // board() is stable: the same board on every call.
    const bsp::Board &first = hal::board();
    CHECK(&hal::board() == &first);
    std::printf("this machine: board %s\n", first.name);

    std::printf(fails ? "probe_test: %d FAILED\n" : "probe_test: all passed\n", fails);
    return fails ? 1 : 0;
}

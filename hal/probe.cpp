#include "hal/probe.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "kestrel_gnd_features.h"

namespace hal {

// The device tree's compatible list, entries joined by spaces ("" off a
// device tree, e.g. on a PC).
static std::string read_compatible() {
    char buf[512] = {};
    size_t n = 0;
    if (FILE *f = fopen("/proc/device-tree/compatible", "r")) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    for (size_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
    return std::string(buf, n);
}

const bsp::Board &pick(const std::string &compat) {
#if KESTREL_HAL_ROCKCHIP
    // The VRX Pro's tree is stock's (fpvOS keeps the vendor's), so it says
    // only what SoC it is. Our own trees will name their box.
    if (compat.find("rockchip,rk3568") != std::string::npos ||
        compat.find("rockchip,rk3566") != std::string::npos)
        return bsp::vrx_pro();
#endif
    (void)compat;
    return bsp::pc();
}

const bsp::Board &board() {
    static const bsp::Board &b = [] () -> const bsp::Board & {
        const std::string compat = read_compatible();
        const bsp::Board &picked = pick(compat);
        printf("board: %s (device tree: %s)\n", picked.name,
               compat.empty() ? "none" : compat.c_str());
        return picked;
    }();
    return b;
}

}  // namespace hal

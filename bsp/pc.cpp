// A PC or any box nothing else matches: no front panel, supply ADC or Open8030
// image. The thermal zone is read where there is one.
#include "bsp/bsp.hpp"

namespace bsp {

static const Board kPc = {
    "pc",
    { { "", 0, 1 }, 0, 0, nullptr, 0 },
    { { "", 0, 1 }, 0 },
    { { "", 0, 1 }, 1, 0, 1 },
    "/sys/class/thermal/thermal_zone0/temp",
    { "" },
};

const Board &pc() { return kPc; }

}  // namespace bsp

// The box this build is for, as KESTREL_BOARD chose it.
#include "bsp/bsp.hpp"
#include "kestrel_gnd_features.h"

namespace bsp {

const Board &board() {
#if KESTREL_BOARD_VRXPRO
    return vrx_pro();
#else
    return pc();
#endif
}

}  // namespace bsp

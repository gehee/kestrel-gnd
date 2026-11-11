#define MINIMP4_IMPLEMENTATION
// Recordings over 4 GiB: finishing one writes its mdat with a 64-bit size,
// past which a demuxer without this never reaches the moov (nor takes co64).
#define MP4D_64BIT_SUPPORTED 1
#include "third_party/minimp4.h"

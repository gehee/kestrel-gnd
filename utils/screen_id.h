#ifndef SCREEN_ID_H
#define SCREEN_ID_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The connected screen's identity, from its EDID: the manufacturer's
// three-letter code and the product code, e.g. "CPO1F41" - one model of
// display, which is what a mode belongs to (the serial number is left out:
// many displays leave it 0, and two of the same model take the same modes).
//
// Read from sysfs, so kestrel-gnd and the splash agree on it without a DRM
// handle: the first connected connector that is not a writeback one, the same
// connector modeset_prepare() drives. Returns 1 and fills out with the ID, or
// 0 with out empty when no display is connected or it sent no usable EDID.
int screen_id(char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif

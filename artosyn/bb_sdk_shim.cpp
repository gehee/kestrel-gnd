// Override the AR8030 SDK's ioctl length table with the newer one extracted
// from stock's ar_ldy_gnd.
//
// Why this is needed
// -----------------
// We link /ar8030soc/libar8030_client.so, but stock's ar_ldy_gnd does NOT -
// it statically embeds a newer SDK build. The daemon on the device shipped
// with ar_ldy_gnd, and the daemon is what actually answers our ioctls, so the
// NEWER table is the one that matches the wire. The shipped .so's table is a
// stale leftover; it disagrees with the daemon in three ways that matter:
//
//   * commands missing entirely       -> bb_ioctl() returns -2 and sends
//                                        nothing (BB_GET_WORK_CHAN_LIST)
//   * iptsize 0 for a real command    -> the command goes out with an empty
//                                        payload, which looks like success
//                                        (BB_SET_WORK_CHAN_LIST)
//   * plain wrong sizes               -> e.g. BB_SET_POWER_AUTO 1 vs 3 bytes
//
// Confirmed at runtime: BB_GET_CHAN_INFO returns >= 1028 bytes, which is what
// ar_ldy_gnd's table says (1028) and not what the .so's says (964).
//
// How it works
// -----------
// libar8030_client.so calls get_bb_ioctl_cmdiptlen() through its PLT: the
// symbol is GLOBAL, there is an R_AARCH64_JUMP_SLOT relocation for it, and the
// library is not linked -Bsymbolic. So a definition in the executable wins at
// load time and the library calls ours instead. This needs the executable to
// export its dynamic symbols (-rdynamic; see CMakeLists.txt) - without that
// the override silently does nothing, which is what bb_sdk_shim_active()
// exists to detect.
//
// Only cmdiptlen is hooked: it is the only one of the three table accessors
// with a JUMP_SLOT reloc, i.e. the only one the library actually calls through
// the PLT. It is also the one that sets the length put on the wire.
//
// Set KESTREL_BB_SDK=legacy to fall back to the library's own table.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

#include "bb_ioctl_v2.h"

namespace {

bool use_legacy() {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("KESTREL_BB_SDK");
        cached = (e && strcmp(e, "legacy") == 0) ? 1 : 0;
    }
    return cached == 1;
}

// The library's own implementation, for commands the newer table lacks.
// RTLD_NEXT skips our definition and finds the one in libar8030_client.so.
int legacy_iptlen(int req) {
    typedef int (*fn_t)(int);
    static fn_t orig = nullptr;
    static bool looked_up = false;
    if (!looked_up) {
        looked_up = true;
        orig = (fn_t)dlsym(RTLD_NEXT, "get_bb_ioctl_cmdiptlen");
    }
    return orig ? orig(req) : -1;
}

int v2_iptlen(int req) {
    for (int i = 0; i < BB_IOCTL_SIZES_V2_N; i++)
        if (bb_ioctl_sizes_v2[i].req == req) return bb_ioctl_sizes_v2[i].iptsize;
    return -1;
}

}  // namespace

extern "C" int get_bb_ioctl_cmdiptlen(int req) {
    if (use_legacy()) return legacy_iptlen(req);
    int v = v2_iptlen(req);
    if (v >= 0) return v;
    // Not in the newer table (a few GET 115-118 / SET 30,31,33 exist only in
    // the older one). Defer rather than fail the command outright.
    return legacy_iptlen(req);
}

// Returns true if our definition is the one the library resolved to. Call at
// startup: a false here means -rdynamic was lost and every "newer SDK" command
// will quietly fail with -2 again.
extern "C" bool bb_sdk_shim_active() {
    if (use_legacy()) return false;
    // BB_GET_WORK_CHAN_LIST exists only in the newer table, so the library's
    // own lookup returns -1 for it while ours returns 0.
    return get_bb_ioctl_cmdiptlen(BB_GET_WORK_CHAN_LIST) >= 0
           && legacy_iptlen(BB_GET_WORK_CHAN_LIST) < 0;
}

extern "C" void bb_sdk_shim_report() {
    if (use_legacy()) {
        printf("ar8030: bb sdk table = legacy (KESTREL_BB_SDK=legacy)\n");
        return;
    }
    printf("ar8030: bb sdk table = v2 (from ar_ldy_gnd), interposed=%s\n",
           bb_sdk_shim_active() ? "yes" : "NO - rebuild with -rdynamic");
}

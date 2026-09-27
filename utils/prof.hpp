#ifndef KESTREL_PROF_HPP
#define KESTREL_PROF_HPP

// HUD render profiler. Off unless KESTREL_PROF is set in the environment
// (the init script passes its environment on), and then every 5 s it prints
// one line per measure to stdout (/var/log/kestrel-gnd.log):
//
//   - time per section of a HUD frame, mean and p99
//   - renders per second, and what woke each one
//   - named event counters (commit paths, refused flips, ...)
//   - the GPU's real completion time for each frame, taken from the kernel's
//     timestamp on the frame's native fence - not from glFinish, which on
//     this Mali splits the render pass and waits out the display, and so
//     measures something production never pays
//
// Disabled, every call is one predictable branch.

#include <cstdint>

namespace prof {

enum Sec  { kSnapshot, kDrawPre, kCanopy, kBfGrid, kDraw, kSwap, kLock, kHandoff, kSecCount };
enum Wake { kWakeVideo, kWakeAnim, kWakeInput, kWakeLink, kWakeTimeout, kWakeOther, kWakeCount };

bool enabled();

// A HUD frame: begin, then mark() at the end of each section in order.
void frame_begin();
void mark(Sec s);
void frame_end();

void wake(Wake w);                 // what asked for a render
void count(int id);                // a named event, see kCount* below
// A native fence fd for this frame's GPU work, submitted at submit_us
// (CLOCK_MONOTONIC us). Takes ownership of fd.
void gpu_fence(int fd, uint64_t submit_us);

enum { kCountOsdCommit, kCountOsdPending, kCountFlipBusy, kCountTextMiss, kCountN };

}  // namespace prof

#endif

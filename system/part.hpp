#pragma once
// A part of the system - the air unit's camera, its radio, our radio, the
// pictures' timing... - as the rest of kestrel-gnd sees it, whatever carries
// it: what it is now, the changes asked of it, what it sends as it comes.
// Each is an interface (system/air/, system/gnd/), called directly from any
// thread; what implements it (radio::Model, for what the radio carries) hides
// how its threads hand over, and nothing a caller does waits on them.
#include <cstdint>

namespace bus { class Waker; }

// A change asked of a part: its number in the order the changes were asked
// (0: none was made), and the value as the part will hold it.
struct Ticket {
    uint32_t seq = 0;
    int value = 0;
};

// What a setting shows: the value asked until the part has carried the
// change out (its state's taken), then the value it holds.
inline int shown(const Ticket& asked, int held, uint32_t taken) {
    return asked.seq > taken ? asked.value : held;
}

// A part's state: the newest, read without waiting; its version counts the
// changes (0: none yet). One watcher, set before the threads start, is
// poked at each change.
template <class S> class Part {
public:
    virtual ~Part() = default;
    virtual S state() const = 0;
    virtual uint32_t version() const = 0;
    virtual void watch(bus::Waker* w) = 0;
};

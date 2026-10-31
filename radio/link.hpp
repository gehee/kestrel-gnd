#pragma once
// What any radio does for kestrel-gnd: runs the link, reports what it learns
// to its model (radio::Model), writes the air unit's streams into the model's
// feeds, and carries out the commands the model hands it; nothing else is
// called on it from other threads. radio/ar8030_link is one; a second radio,
// a replay or a test fake would be others.
#include <cstdint>
#include <string>
#include <vector>

#include "radio/radio.hpp"

namespace radio {

class Link {
public:
    virtual ~Link() = default;

    // Runs the link until stopped; on the thread that calls it, the radio
    // thread. Between its steps it calls Model::take_commands(*this).
    virtual void run() = 0;
    // One command, on the radio thread (from Model::take_commands).
    virtual void apply(const Command& c) = 0;

    // Its settings as it holds them now (State, its commands_taken left to
    // the model): on the radio thread, after its commands (Model::take_commands).
    virtual State held() const = 0;
};

}  // namespace radio

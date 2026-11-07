#pragma once
#include <string>

// A per-radio-frame log on the card, written by its own thread so nothing
// that feeds it waits on the card (the radio's stats thread). Text is kept in
// memory and written, then synced, every 2 s; a card more than 1 MB behind
// loses text, never time. A new path closes the old file and starts the new one
// (with its header line).
namespace frame_log {
void append(const std::string& path, const std::string& header, std::string lines);
}

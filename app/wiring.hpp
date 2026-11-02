#pragma once
// Startup wiring: the radio and what reads it, made and attached in one place
// before the radio runs. The radio reports to its model (radio::Model), which
// implements the parts it carries (system/); the HUD was handed those parts
// when it was made, the IMU reader and the DVR (a video sink) are attached
// here. The decoder is the radio's own direct call (slice-timed decoding
// cannot wait on anything).
#include <atomic>
#include <memory>

#include "base/settings.hpp"
#include "base/video_codec.hpp"

class Vdec;
class OSD;
struct Ar8030Config;
namespace radio { class Model; }

// Runs the radio on this thread until stop is set.
void run_radio(radio::Model& model, const Ar8030Config& cfg, Settings& settings,
               const VideoCodec& codec, std::shared_ptr<Vdec> vdec,
               std::shared_ptr<OSD> osd, std::atomic<bool>* stop, int slot, int port);

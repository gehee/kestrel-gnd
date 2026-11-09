#pragma once
#include "base/bus.hpp"
#include <memory>
#include <vector>
// A stream recording destination, independent of screen capture hardware.
// DvrSink shares keyframe recovery and HEVC repair across receiver platforms.
class RecordingTarget {
public:
    virtual ~RecordingTarget() = default;
    virtual bool is_recording() const = 0;
    virtual void set_parameter_sets(const std::vector<std::vector<uint8_t>>&) = 0;
    virtual void feed(std::shared_ptr<std::vector<uint8_t>>,uint64_t us) = 0;
    virtual void feed(const bus::PictureRef&,uint64_t us) = 0;
    virtual uint64_t session() const { return 0; }
};

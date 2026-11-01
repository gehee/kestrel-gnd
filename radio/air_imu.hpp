#pragma once
// The air unit's IMU samples, as kestrel-air --imu sends them: H.265 SEI messages
// (user_data_unregistered, UUID "kestrel-air-IMU1") after the last slice of each
// picture. The radio reads them out here and publishes them (air::ImuBatch);
// motion/imu_stream turns them into the camera's orientation.
//
// Samples carry the air unit's CLOCK_MONOTONIC in microseconds, the same clock as the
// capture stamp in each picture's radio header.
//
// Layout of the SEI payload (little-endian), written by kestrel-air src/sky/imu.c:
//   UUID | u8 version 1 | u8 flags | u16 picture | u64 capture PTS | u64 first sample time |
//   u16 n | u16 gyro LSB per 0.1 dps | u16 accel LSB per g | n x { u16 us after the first,
//   i16 gx gy gz ax ay az }
#include <cstddef>
#include <cstdint>
#include <functional>

#include "system/air/imu.hpp"

namespace radio {

// One SEI NAL unit, its 2-byte header first, no start code. Each IMU message in it
// goes to emit as one or more batches (air::ImuBatch::kMax samples each), its
// samples filled in; rx_ms and camera_angle are the caller's. True if it was the
// air unit's IMU.
bool read_imu_sei(const uint8_t* nal, size_t len, const std::function<void(air::ImuBatch&)>& emit);

}  // namespace radio

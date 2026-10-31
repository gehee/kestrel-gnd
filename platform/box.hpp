#pragma once
// What each box's file (platform/<box>.cpp) says for the code its boxes share
// (platform/caps.cpp).
namespace platform {

// Refresh Sync: the box's display controller can lengthen a frame (the VOP2's
// frame length, hal/frame_length).
bool has_refresh_sync();

}  // namespace platform

#include "base/frame_mode.hpp"

std::atomic<int> g_frame_mode{kFrameWhole};
std::atomic<int> g_camera_fps{0};
std::atomic<int> g_camera_lines{0};

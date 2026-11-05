#pragma once
#include <cstdint>
#include <ctime>
namespace kestrel_ui {
inline uint64_t monotonic_us() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
}

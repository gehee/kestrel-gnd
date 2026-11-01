#include "base/restart.hpp"

#include <atomic>

static std::atomic<void (*)()> g_restart_handler{nullptr};

void set_restart_handler(void (*fn)()) { g_restart_handler.store(fn); }

void kestrel_request_restart() {
    if (auto fn = g_restart_handler.load()) fn();
}

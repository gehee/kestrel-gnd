#pragma once
// The process's signals: Ctrl-C and SIGTERM stop it (a second one, or a stop
// wedged for 5 s, forces it), SIGUSR1 starts or stops a recording, a crash
// prints where it was; and a restart asked for (base/restart.hpp), which is
// the ordinary stop and then the same binary again.

#include <atomic>

#include "base/settings.hpp"

extern std::atomic<bool> signal_stop;   // set when the app is to stop

// First thing in main(): its argv for the restart's exec, and the settings
// file, written before it.
void signals_set_restart(char** argv, Settings& settings);
// The handlers, once the command line is read.
void signals_install();
// A recording toggled by SIGUSR1 since the last call (the button thread acts
// on it: a signal handler cannot open files).
bool signals_take_rec_toggle();
// A restart was asked for; main() ends with signals_restart_now(), an exec.
bool signals_restart_asked();
void signals_restart_now();

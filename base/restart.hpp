#pragma once
// Stop the app the ordinary way and start it again in place: for a setting
// read only at start, such as the screen mode. How is app/main.cpp's, which
// sets it before anything can ask; until then asking does nothing.
void kestrel_request_restart();
void set_restart_handler(void (*fn)());

// The same binary exec'd in place (a restart, the bring-up watchdog) gets
// what the process had when it started, not what it made of it since: the
// exec keeps the calling thread's signal mask, scheduling and CPUs, every
// ignored signal, a pending alarm and every descriptor not marked CLOEXEC.
//
// First thing in main(): every signal unblocked (an exec from a signal
// handler hands its signal on blocked) and the start state kept.
void restart_init();
// Right before execv("/proc/self/exe"), from any thread or a signal handler
// (async-signal-safe): stdout and stderr back from the SD log's pipe, no
// alarm pending, the signal dispositions, mask, scheduling and CPUs the
// process started with, every descriptor above stderr closed.
void restart_prepare_exec();

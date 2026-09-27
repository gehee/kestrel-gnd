#ifndef BB_WATCHDOG_HPP
#define BB_WATCHDOG_HPP

// Bring-up watchdog for a bb_socket_open() that never returns (an untimed
// condition wait inside the vendor SDK). Recovery ladder, lightest first:
// ask the daemon to force-close every socket over a fresh RPC connection
// (no SDK involved, no link dropped); then re-exec the process; then restart
// the daemon. See bb_watchdog.cpp for the reasoning behind each rung.
// 0 disables the watchdog, 1 selects the built-in default, anything larger is
// the deadline in seconds. Call before bb_watchdog_start(); overrides the
// KESTREL_BB_WATCHDOG_S environment variable.
void bb_watchdog_set_deadline(int seconds);
// True once a deadline has been set explicitly, so settings do not overwrite
// what the command line asked for.
bool bb_watchdog_deadline_set();
void bb_watchdog_start(char **argv);   // call once, with main's argv
void bb_watchdog_open_begin();         // entering the link bring-up
void bb_watchdog_open_end();           // it returned, success or failure
// Called once per pass of the AR8030 run loop. The SDK can park a thread on an
// untimed condition wait in calls other than the bring-up, and a watchdog that
// only brackets connect_bb() cannot see those at all - the process connects,
// then silently stops. A heartbeat covers every one of them.
void bb_watchdog_alive();

// Scope guard for the bring-up, so an early `return false` cannot leave the
// watchdog believing we are still in there.
struct BringupGuard {
    BringupGuard()  { bb_watchdog_open_begin(); }
    ~BringupGuard() { bb_watchdog_open_end(); }
};
void bb_watchdog_socket_open();        // the video socket opened
void bb_watchdog_stop();               // shutting down on purpose

#endif

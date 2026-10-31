#include "app/signals.hpp"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "base/restart.hpp"
#include "base/sd_log.hpp"

volatile bool signal_stop = false;

// The settings file, written before a restart.
static Settings* g_settings = nullptr;

// Ctrl-C has to work even when a thread is wedged inside the AR8030 SDK.
// ar_cfg.run() executes on the main thread, and bb_init()/bb_socket_open()
// block with no timeout whenever the baseband is not healthy (a previous client
// killed with SIGKILL leaves the daemon's single owner slot taken). In that state
// nothing is left alive to notice signal_stop, so a plain flag-setting handler
// leaves the process unkillable by anything short of SIGKILL.
//
// So: first signal asks for a graceful stop and arms a watchdog; a second signal,
// or the watchdog firing, exits immediately. Everything below is
// async-signal-safe (write/_exit/alarm), unlike printf.
static volatile sig_atomic_t stop_requested = 0;
#define SIG_MSG(m) do { ssize_t r_ = write(STDERR_FILENO, m, sizeof(m) - 1); (void)r_; } while (0)

static void force_exit(int signum)
{
	(void)signum;
	SIG_MSG("\nkestrel-gnd: forced exit (shutdown wedged, most likely in the BB SDK)\n");
	_exit(130);
}

// kill -USR1 <pid>: start/stop a recording. Same entry point as the REC button.
// The button was the only way in, which made the DVR impossible to exercise
// over ssh or from a script - and that is how several DVR bugs stayed hidden.
//
// This was documented here and never registered, so SIGUSR1 kept its default
// disposition and killed the process instead. Anyone following the comment to
// test the DVR remotely just watched the app die.
static volatile sig_atomic_t g_rec_toggle = 0;
static void rec_toggle_handler(int) { g_rec_toggle = 1; }
// A restart, for a setting that is only read at start - the screen mode: the
// display is set up once. The ordinary shutdown runs, so a recording is
// stopped and finalized and the radio's sockets are closed, and main() then
// execs the same binary where it would have returned. Should that shutdown
// hang, the alarm that would force an exit re-execs instead: a restart must
// never end with no app on the screen. Everything here is async-signal-safe.
static volatile sig_atomic_t g_restart = 0;
static char** g_main_argv = nullptr;
static void reexec_now(int)
{
	sd_log::restore_output();   // the new process starts its own
	for (int fd = 3; fd < 1024; fd++) close(fd);
	execv("/proc/self/exe", g_main_argv);
	_exit(91);
}
static void request_restart()
{
	if (stop_requested) return;
	// The setting that asked for the restart (a screen mode on trial) must be
	// on disk for the new process, and the forced re-exec below cannot write it.
	g_settings->flush();
	g_restart = 1;
	stop_requested = 1;
	signal_stop = true;
	signal(SIGALRM, reexec_now);
	alarm(5);
}

static void sig_handler(int signum)
{
	if (stop_requested) force_exit(signum);   // second Ctrl-C: go now
	stop_requested = 1;
	signal_stop = true;
	SIG_MSG("\nkestrel-gnd: stopping (Ctrl-C again to force)\n");
	// If the graceful path has not finished in 5 s it is blocked in a call that
	// cannot be interrupted; take the process down rather than hang.
	signal(SIGALRM, force_exit);
	alarm(5);
}


// A code address as library+offset, which addr2line takes; or that it is in
// no library at all.
static void crash_addr(const char* what, void* a) {
    Dl_info di;
    if (a && dladdr(a, &di) && di.dli_fname)
        fprintf(stderr, "  %s %p = %s+0x%lx (%s)\n", what, a, di.dli_fname,
                (unsigned long)((uintptr_t)a - (uintptr_t)di.dli_fbase),
                di.dli_sname ? di.dli_sname : "?");
    else
        fprintf(stderr, "  %s %p (in no library)\n", what, a);
}

// The registers come first. A call through a corrupted pointer - heap
// corruption, as the renderer's stats overflow once caused - leaves the PC in
// no library, and the backtrace stops right there with nothing to go on; the
// link register still says who made the call, and the thread's name whose
// thread it was.
static void segv_bt(int sig, siginfo_t* si, void* ucv){
    char name[17] = {0};
    prctl(PR_GET_NAME, name);
    fprintf(stderr, "\n### %s in thread %s, fault address %p ###\n",
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT", name, si->si_addr);
#if defined(__aarch64__)
    const mcontext_t& mc = ((ucontext_t*)ucv)->uc_mcontext;
    crash_addr("pc", (void*)mc.pc);
    crash_addr("lr", (void*)mc.regs[30]);
    fprintf(stderr, "  sp %p\n", (void*)mc.sp);
#else
    (void)ucv;
#endif
    void* bt[48]; int n=backtrace(bt,48);
    fprintf(stderr,"### backtrace (%d frames) ###\n", n);
    backtrace_symbols_fd(bt,n,2);
    signal(sig,SIG_DFL); raise(sig);
}

void signals_set_restart(char** argv, Settings& settings) {
	g_main_argv = argv;   // for a restart (kestrel_request_restart)
	g_settings = &settings;
	set_restart_handler(request_restart);
}

void signals_install() {
	signal(SIGINT, sig_handler);

	// The DVR toggle the comment above promised. Without this, SIGUSR1 keeps

	// its default disposition and terminates the process.

	signal(SIGUSR1, rec_toggle_handler);
	signal(SIGTERM, sig_handler);   // systemctl stop / killall, same watchdog
	{
		struct sigaction sa = {};
		sa.sa_sigaction = segv_bt;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
		sigaction(SIGABRT, &sa, nullptr);
	}
	// Ignore SIGPIPE — a broken socket (e.g. the RPC link when the gnd radio
	// reboots, as a channel pin does) must surface as an EPIPE the send/recv
	// paths handle + reconnect from, not tear the whole app down.
	signal(SIGPIPE, SIG_IGN);
}

bool signals_take_rec_toggle() {
	if (!g_rec_toggle) return false;
	g_rec_toggle = 0;
	return true;
}

bool signals_restart_asked() { return g_restart; }

void signals_restart_now() { reexec_now(0); }

#pragma once
#include <string>

// The log on the recordings partition: everything kestrel-gnd prints (stdout
// and stderr) also goes to <dir>/fpvOS_YYYYMMDD_HHMMSS.log, named as the
// recordings are, so a flight's log survives the goggle being switched off
// (/var/log is in RAM). The service's own log keeps everything as before.
//
// Nothing that prints waits on the card: a thread passes each line on to the
// original output and keeps it in memory; another writes that to the card and
// syncs every 2 s. A card that falls more than 1 MB behind loses text, never
// time. The newest kKeep logs are kept: each starts with its number
// ("sd-log: log #N"), as the clock may not say which is newest.
//
// Only while HUD > Debug Info is on: each time it is turned on, a new log
// from then on.
namespace sd_log {
// Where the logs go; nothing is written before set_enabled(true).
void init(const std::string& dir);
// On: a new log, if dir is a mounted partition (not the plain mount point;
// says why not otherwise). Off: what is pending written, the log closed.
void set_enabled(bool on);
// The log being written now (empty while off): others name their files after
// it (the radio's per-frame log, <log without .log>.frames.csv).
std::string current_path();
// stdout and stderr back to where they went before the first log: for a re-exec,
// whose new process has none of these threads (async-signal-safe).
void restore_output();
// What was printed so far, onto the card now (the reader is given up to
// 300 ms to empty the pipe): before the process ends or execs, as these
// threads do not outlive it. may_wait false - from a signal handler, or a
// thread that must not hang on the card: no lock is waited for (a part held
// by a thread is skipped), nothing allocated, no stdio.
void flush(bool may_wait = true);
// Where a crash report is written straight to, as the process dies before
// the reader would pass it on: the output before the redirect (stderr while
// there is none) and the card's log (-1 while off). Async-signal-safe.
void crash_fds(int* out, int* card);
}

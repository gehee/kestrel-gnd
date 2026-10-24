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
// time. The newest kKeep logs are kept.
namespace sd_log {
// Starts it if dir is a mounted partition (not the plain mount point); says
// why not otherwise.
void start(const std::string& dir);
// stdout and stderr back to where they went before start(): for a re-exec,
// whose new process has none of these threads (async-signal-safe).
void restore_output();
}

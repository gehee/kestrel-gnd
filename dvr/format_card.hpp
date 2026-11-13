#pragma once
// DVR > Format Card: the firmware's fpvos-format-dvr does the work - the
// recordings partition on a VRX Pro (its boot card), the whole removable card
// on the AR9311 receivers. kestrel-gnd only checks, asks, and shows how it
// went: at its next start where the helper stops and starts it around the
// format (the card holds its settings, or it is the VRX Pro's own card), or
// by reading the outcome while it keeps running (take_result, polled).
#include "base/shell.hpp"
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace dvr::format_card {

inline constexpr const char* kHelper = "/usr/sbin/fpvos-format-dvr";
inline constexpr const char* kStatus = "/run/fpvos-format-dvr.status";
// The helper's one line of output (run_shell gives no pipe).
inline constexpr const char* kOutput = "/run/fpvos-format-dvr.out";
// Two presses: the first arms it for this long, the second erases - but not
// sooner than kGapMs after the first, so a double press does not erase before
// the warning can be read. Leaving the row disarms it.
inline constexpr unsigned kArmMs = 5000;
inline constexpr unsigned kGapMs = 800;

struct Result {
    bool ok = false;
    std::string text;   // what would be erased, or why it cannot be
};

namespace detail {
inline Result run(const char* verb) {
    Result r;
    if (access(kHelper, X_OK) != 0) { r.text = "Needs updated fpvOS firmware"; return r; }
    // run_shell, not popen: the helper's worker outlives this process and
    // starts the next kestrel-gnd, and must hold none of our descriptors (the
    // decoder's buffers, the screen, a file on the card it is about to unmount).
    const std::string command = std::string(kHelper) + " " + verb + " >" + kOutput + " 2>/dev/null";
    const int status = run_shell(command.c_str());
    char line[160] = {};
    if (FILE* f = fopen(kOutput, "re")) {
        if (!fgets(line, sizeof(line), f)) line[0] = '\0';
        fclose(f);
    }
    unlink(kOutput);
    r.text = line;
    while (!r.text.empty() && (r.text.back() == '\n' || r.text.back() == '\r')) r.text.pop_back();
    r.ok = status == 0;
    if (r.text.empty()) r.text = r.ok ? "Ready" : "Not available";
    return r;
}
} // namespace detail

// What the card (or partition) is, or why it cannot be formatted. Runs a
// short shell script: call it on a tab opening, not every frame.
inline Result check() { return detail::run("check"); }
// Starts the format in the background; where the helper needs kestrel-gnd
// stopped, that happens shortly after (its text then says it restarts).
inline Result start() { return detail::run("start"); }

// How the last format went, once: "" when there is nothing to say (none,
// or still running). The status file is removed after it has been read.
inline std::string take_result() {
    FILE* f = fopen(kStatus, "re");
    if (!f) return {};
    char line[200] = {};
    if (!fgets(line, sizeof(line), f)) line[0] = '\0';
    fclose(f);
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.rfind("running", 0) == 0) return {};
    unlink(kStatus);
    if (s.rfind("ok ", 0) == 0) return s.substr(3);
    if (s.rfind("failed ", 0) == 0) return "Format failed: " + s.substr(7);
    return {};
}

} // namespace dvr::format_card

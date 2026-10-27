#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The DVR folder as the web gallery and the goggle's own gallery both see it.
namespace dvr_lib {

// A recording name that is safe to touch: a plain file name, nothing that
// could climb out of the DVR folder.
bool safe_name(const std::string& n);
bool ends_with(const std::string& s, const char* suffix);

std::string dir();                                    // the dvr_dir setting
std::string thumb_dir();                              // <dir>/.thumbs
std::string thumb_path(const std::string& name);      // <dir>/.thumbs/<name>.jpg

// Length of an MP4 in seconds, or -1 (a recording still being written, or one
// that was cut short and has no readable length).
double mp4_duration(const std::string& path);

// Make a thumbnail of a recording in the background, if it has none yet. A
// no-op for one that is queued or being made.
void request_thumbnail(const std::string& name);

struct Recording {
    std::string name;
    uint64_t    size;
    int64_t     mtime;
};

// Delete a recording and its thumbnail, as the web gallery does: 0 when it is
// gone, else the errno (EINVAL: not a recording name, EBUSY: `active`, the one
// being written, ENOENT: not there, or what unlink said).
int delete_recording(const std::string& name, const std::string& active);

// The recordings (.mp4 and .h265), newest first.
std::vector<Recording> list_recordings();

// The "YYYYMMDD_HHMMSS" in a recording's name, or its mtime in that form.
std::string stamp_of(const Recording& r);

} // namespace dvr_lib

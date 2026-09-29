#ifndef DVR_RECOVER_H
#define DVR_RECOVER_H

#include <functional>
#include <string>

// Recordings left fragmented, made plain in place.
//
// The DVR records fragmented MP4 - a power cut loses a second at most - and
// makes the file plain when recording stops (MP4E_set_finalize): a phone
// browser will not play a fragmented file from its URL. A recording that
// never stopped - power cut, crash - stays fragmented, as does every one made
// before kestrel finalized them. This finds those in dir and finalizes them:
// a half-written last picture is dropped, and the decoder configuration is
// completed from the stream's own SPS (earlier kestrels wrote HEVC level 0
// and a monochrome chroma format there).
//
// Safe to stop at any point and run again: the steps are ordered so that a
// file cut short part way is still one of the two - fragmented and a
// candidate again, or finished.
//
// `busy` names the file being recorded (a basename, or ""), asked before
// each file; that one is left alone.
bool dvr_finalize_fragmented(const std::string& path);   // false: not one, or failed
void dvr_recover_recordings(const std::string& dir, const std::function<std::string()>& busy);

#endif

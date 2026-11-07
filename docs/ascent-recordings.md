# Native Ascent recordings

The recorder saves the received HEVC stream in MP4 without decoding or
re-encoding it. Two optional companion files provide gallery and playback UI:

- `.thumbs/<video filename>.jpg`: 320×180 preview, generated once from an
  already decoded frame. Existing videos acquire a preview on first playback.
- `<video filename>.telemetry.jsonl`: versioned, timestamped telemetry track.

A valid cached JPEG is reused on repeat playback without sampling another frame.
The thumbnail worker holds at most one additional decoder buffer, samples it
with DMA-BUF CPU synchronization, then releases it before JPEG encoding and
storage. Gallery loading, JPEG decoding, and sizing run on that worker. The
view requests six visible previews; the worker cache is capped at twelve.
Images are prepared for the active 720p or 1080p layout once, not scaled on
each redraw. Missing/corrupt previews are placeholders. Thumbnails do not
require FFmpeg or a second video decoding session.

## Telemetry format

The first JSON line identifies the format:

```json
{"format":"kestrel.telemetry","version":1,"timebase":"mp4_microseconds","hud_codec":1}
```

Subsequent lines have `t_us`, `type` (`snapshot` or `update`), and `data`.
The first accepted MP4 sample has timestamp zero. Timestamps use the same
accumulated sample durations that the muxer writes, including repaired frames
and variable cadence. They do not subtract unrelated air/receiver clocks.
Each access unit takes a reference to the latest UI telemetry snapshot as it
enters the recorder queue. This reproduces the receiver-observed state at
recording time; it does not claim sensor-time synchronization for telemetry
whose original measurement time is not available.

Full snapshots occur at least every two seconds while frames are recorded.
Between them only changes are written:

- `values`: numeric readings, flags, and motion inputs, with named units.
  `flight.*` includes received flight values and individual availability flags;
  `flight.fresh`, `vrx.fresh`, `vrx.link_valid`, `video.fresh`, and `osd.fresh`
  preserve freshness separately from the last value. A null value removes a key.
- `text`: flight mode, FC/VTX identities, and the clock text seen during recording.
- `grid`: complete 53×20 Betaflight OSD glyph canvas in snapshots.
  `grid_updates`: `[cell index, glyph]` pairs in updates.
- `hud_v1`: hexadecimal encoding of version 1 display-ready HUD data, using
  `hud/lvgl/hud_protocol.cpp`. This preserves calculated sag, warning states,
  timers and formatted readings without recomputing them from current live data.
  A future incompatible HUD codec must use a new version.
- `motion.dx`, `motion.dy`, `motion.bank_rad`: the resulting Canopy offsets in
  world coordinates and rotation in radians, independent of display resolution.
  `motion.reactivity` records the selected movement setting.
- `imu.*`: sampled camera angular rates (20 ms average, degrees/second) and
  integrated orientation quaternion, with validity flags. These are the
  movement inputs available to the HUD, not an archival dump of every raw IMU
  packet. Flight-controller pitch/roll are retained when received.

The UI publishes snapshots at its update cadence. A bounded worker queue
performs JSON conversion and buffered disk writes, flushing at one-second
video intervals and when recording finishes. Telemetry failure is reported
separately and does not stop video recording. Empty recordings remove their
empty sidecars. The catalog delete operation removes both companion files.

## Playback

The hardware decoder receives MP4-relative presentation timestamps. Completed
video flips expose those timestamps to the application; the HUD selects the
recorded state for the frame actually displayed. The player parses the sidecar
on its worker and retains a bounded 128-frame history for this lookup. Motion
can interpolate between nearby samples; discrete readings and warnings hold
until their next update. Pause holds the picture, readings, motion, and warning
pulse time. Current live receiver readings are never substituted for a missing
recorded HUD.

LEFT/RIGHT seek backward/forward five seconds; held arrows repeat when key repeat
is enabled. The timeline shows the current position and the requested position
while seeking. Requests clamp to the beginning or last frame, and repeated
requests accumulate while a seek is pending. A paused seek displays the target
frame and stays paused; a playing seek continues playback.

Seeking recreates only the hardware decoder, preserving the HDMI mode and UI.
The player finds the preceding HEVC random-access frame, feeds its reference
frames as fast as hardware decode permits with at most six outstanding pre-roll
frames, and suppresses their display until the target frame. Clips with long gaps
between keyframes take longer to seek because those reference frames must decode.
The telemetry reader restores the matching recorded state, including backward
seeks, and old playback HUD data is hidden during the transition.

UP toggles the recorded HUD during playback. OK pauses/resumes; BACK returns to
the gallery. The initial HUD setting follows the current HUD preference. The
current theme, font scale, and detail preference apply to the recorded readings.
Legacy recordings without a track show `HUD UNAVAILABLE` and play normally.
Malformed or unsupported sidecars disable the recorded HUD while video continues.
A truncated final JSON line is ignored; a missing telemetry tail expires after
2.5 seconds rather than remaining falsely current.

Ascent's Dynamic HUD applies recorded horizontal and vertical offsets while
keeping the panels level, matching live display behavior at both 720p and 1080p.
The selected Dynamic HUD setting must be enabled for playback movement; OFF
centers the panels. `motion.bank_rad` remains in the sidecar for compatibility
and other consumers, but does not rotate the native software HUD.

The sidecar reader keeps checkpoint offsets for backward lookup. Ordinary
external players display clean video; burning the HUD into an export is not
part of recording or playback.

## Performance boundary

Recording adds metadata and one thumbnail, not video composition. Playback
retains independent hardware video and UI planes. HUD translation redraws only
affected regions; its render cost can still prevent a 60 Hz UI cadence at 1080p.
The timeline refreshes at 10 Hz and invalidates only the playback header while
the scene is unchanged. A paused, unchanged timeline does not trigger redraws.

# Native Ascent recordings

The recorder saves the received HEVC stream in MP4 without decoding or
re-encoding it, as `fpvOS_YYYYMMDD_HHMMSS.mp4` like the VRX Pro. The card is
kept as the VRX Pro keeps it: REC start clears 300 MiB, and while recording the
writer keeps a minute of the recording's rate free, deleting the oldest
finished recordings (`fpvOS_`, and `kestrel_`/`recording_` from earlier builds)
with their companion files. A full card with nothing left to delete ends the
recording with "card full". Two optional companion files provide gallery and
playback UI:

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

In the gallery, REC opens a deletion confirmation for the selected clip.
LEFT selects CANCEL, RIGHT selects DELETE, and OK confirms the highlighted
choice. CANCEL is selected initially; BACK or REC dismisses the prompt.
The confirmation stays tied to the original filename if the catalog refreshes.
Deletion removes the video, preview and telemetry sidecar on the storage worker,
then refreshes the gallery. Recording/saving and playback must finish first.

The hardware decoder receives MP4-relative presentation timestamps. Completed
video flips expose those timestamps to the application; the HUD selects the
recorded state for the frame actually displayed. The player parses the sidecar
on its worker and retains a bounded 128-frame history for this lookup. Motion
can interpolate between nearby samples; discrete readings and warnings hold
until their next update. Pause holds the picture, readings, motion, and warning
pulse time. Current live receiver readings are never substituted for a missing
recorded HUD.

LEFT/RIGHT request a five-second move, then jump directly to a keyframe in that
direction, matching VRX Pro. RIGHT lands on the keyframe at or after the requested
time; LEFT lands on the keyframe at or before it. Actual jumps can be much larger
in recordings with sparse keyframes. If there is no later keyframe, RIGHT leaves
playback unchanged; LEFT clamps to the first keyframe.

The timeline shows the actual keyframe destination while seeking. Held arrows
repeat when key repeat is enabled, and requests accumulate from the pending
keyframe destination. A paused seek displays that keyframe and stays paused;
a playing seek continues playback. The player indexes sample offsets and scans
NAL headers once per clip, so arrow handling only searches keyframe timestamps.

Seeking resets only the decoder when needed, preserving the HDMI mode and UI.
It does not decode the intervening pictures to reach an exact five-second target.
No extra decoder or decoded-frame cache is used. The telemetry reader restores
the recorded state at the destination, including backward jumps, and old
playback HUD data stays hidden during the transition.

DOWN hides or shows the playback top bar and progress timeline together, without
changing the recorded HUD. Seeking and pause controls remain active while the
bar is hidden. Each new clip starts with the bar visible; holding DOWN does not
repeatedly toggle it.

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

## HUD export implementation

The initial export backend is `hal/artosyn/ge2d_compositor.*`. It imports decoded
YUV DMA buffers and separate ARGB HUD panels, clips translated panels to the
output bounds, and asks fpvOS's native GE2D driver to compose into a separate
encoder input buffer. It never modifies decoder reference pictures. The kernel
UAPI copy in `hal/artosyn/ar9311_ge2d.h` must match fpvOS's `ar9311-ge2d` package.

This backend supports BT.709 limited-range planar YUV and straight-alpha ARGB up
to 1920x1080. Calls are synchronous; callers retain all buffers until return and
must not queue unsuccessful output to an encoder. The bitmap path passes YUV
through RGB, which can round or clip out-of-gamut colors. Clean-video exports
should bypass composition. An engine timeout disables composition until reboot.

`ascent-export-probe` (720p) and `ascent-export-probe 1080` exercise imported DRM
buffers, clipped panels and invalid requests without changing HDMI's mode or
acquiring DRM master. `ge2d_compositor_test` checks request layout and clipping on
the host. The export worker, encoded MP4 output and gallery controls are separate
integration work; this initial backend does not expose an end-user export action.

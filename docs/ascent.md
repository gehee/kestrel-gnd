# Native Ascent VRX

The initial Ascent target runs on native AR9311 fpvOS. It reuses the LVGL
Canopy/Vector frontend from the development branch and the current radio model,
Annex-B assembly, and air-unit protocol. It does not depend on the stock media
service, Rockchip MPP, FFmpeg, Cairo, EGL or GLES.

The composition root is `app/ascent_main.cpp`. `platform/ascent.cpp` supplies a
Wave5 V4L2 decoder and an atomic DRM presenter with independent planes.
Decoded I420 buffers are exported as DMA-BUF and imported directly into the
video plane, including the decoder's coded-height chroma offsets. LVGL publishes
ARGB8888 to a separate UI plane. The VO display hardware performs alpha blending;
there is no per-video-frame CPU copy, RGB conversion or software composition.

Three UI scanout buffers separate uploading, pending and active ownership. UI
uploads run on the UI thread; a stalled UI leaves its last completed buffer
visible while video continues. One KMS thread commits the latest video and UI
buffers and holds decoder leases through flip completion. Completed frames are
presented; partial-slice scanout is not supported. Superseded pictures are dropped
and video is blanked after two seconds without a decoded frame. Fresh decoded
video is shown even when RF telemetry is unavailable.

Compressed input absorbs short SDIO read bursts, with an 8 MiB byte budget
and a 150 ms pending-age limit. A separate 128-picture cap bounds metadata for
tiny access units; narrow-band radio reads can deliver dozens together. Overflow or decoder
errors request a fresh keyframe rather than accumulating stale video. Diagnostic
logs distinguish queue overflow from hardware decode errors.

This requires fpvOS's `0018-drm-artosyn-ui-overlay.patch`. Older single-plane
firmware produces a clear startup error; this target does not silently fall back
to CPU blending.

## Build

Use a Linux AArch64 C/C++ toolchain compatible with the installed fpvOS runtime,
DRM UAPI headers, and ar_libre built with the same toolchain. A fully static build
is also possible. Configure with:

```sh
cmake -S . -B .local/build-ascent \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release -DKESTREL_HAL=ar9311 \
  -DAR8030_LIB="$ARLIBRE_LIBRARY" \
  -DDRM_UAPI_INCLUDE_DIR="$DRM_HEADERS" \
  -DKESTREL_LVGL_SOURCE_DIR="$LVGL_SOURCE"
cmake --build .local/build-ascent -j
```

LVGL is pinned to 9.2.2. Omit `KESTREL_LVGL_SOURCE_DIR` to fetch the pinned
revision. Generate the 1920x1080 HUD assets on the build host using the software
HUD tools documented in the main README; stage those assets beside the receiver
binary. `--assets` selects their directory. No hardware-specific host addresses,
serial ports, power-controller IDs or pairing addresses belong in the build.

The idle screen is one static image generated during the build from the VRX Pro
source font/background and `hud/hud_theme.cpp`. Configure these inputs once:

```sh
cmake -S . -B .local/build-ascent \
  -DKESTREL_UI_FONT="$CHAKRA_FONT" \
  -DKESTREL_UI_BACKGROUND="$FPVOS_BACKGROUND_PNG"
cmake --build .local/build-ascent -j
```

The `kestrel-idle-assets` target runs automatically, including in cross builds.
It uses a **host** C++ compiler, Cairo and Fontconfig development libraries;
none are needed on the receiver. `tools/ui_idle_bake.cpp` defines the layout,
wordmark, shadows and caption. Editing that source, the theme table, font or
background rebuilds `ui-assets/idle-scenes.kui`. The default size is 1920x1080;
`KESTREL_UI_WIDTH` and `KESTREL_UI_HEIGHT` select another software UI size.

Stage `idle-scenes.kui` with the existing Canopy masks and font atlas, or use
`cmake --install` to install the generated file under `share/kestrel/ui`.
The bundle contains every theme; only the selected image occupies runtime RAM
(about 8 MiB at 1080p). The artwork stays centered when either menu opens, and
menu navigation does not regenerate it. Live acquisition messages remain
separate. The old `idle-background.kui` / logo masks are no longer used.

Builds without both artwork inputs omit the bundle; minimal installs fall back
to a fixed text wordmark. Invalid bundles, missing themes and resolution
mismatches are rejected. Generated files belong in `.local/`, not Git.

For tests without hardware, configure the same target with a host ar_libre,
`-DKESTREL_TESTS=ON` and `-DKESTREL_UI_TEST_ASSETS` pointing at generated assets.

## Compiled resolution profiles and screen selection

Supplying `KESTREL_UI_FONT` and `KESTREL_UI_BACKGROUND` also builds
`ui-assets/1280x720` and `ui-assets/1920x1080`, plus a shared `menu-fonts`
directory. Install all three under the directory passed to `--assets`. Each
profile contains its native-size idle scenes, canopy artwork, and Betaflight
atlas. Menu fonts are baked at integer pixel sizes 8 through 48 and loaded on
demand. Refresh rates reuse the same resolution assets. Legacy flat 1080p asset
directories still work.

SYSTEM > Screen Mode supports 720p50/60/100/120 and 1080p50/60. It offers
only modes accepted by the installed DRM driver, advertised by the connected
monitor, and backed by compiled resolution assets. Each candidate receives a
test-only atomic commit, including the fixed 1080p-to-720p video path.
`--force-display` bypasses monitor detection/EDID but still requires driver
acceptance; this is useful for development and does not guarantee a visible
picture on an incompatible sink. Switching recreates the display and receiver
session; stop recording/playback first. Select KEEP within 15 seconds to save
both resolution and refresh rate. An unconfirmed or failed trial restores the
previous complete mode. A saved mode that fails on startup falls back to
1080p60. Existing resolution-only settings retain 60 Hz.
`--screen-mode 1280x720@120`, for example, overrides the saved startup choice.
The other accepted rates are 50, 60 and 100 at 720p, and 50 or 60 at 1080p.

Output refresh rate is independent of the UI frame-rate preference and source
video rate. A 120 Hz output can repeat a 60 FPS video/UI frame; it does not
cause extra rendering of the static idle image or duplicate asset bundles.

The video remains a 1080p decoded DMA-BUF. A compatible 720p driver must scale
that primary plane in hardware while scanning out the UI at native 1280x720.
There is no software resizing of the complete UI or camera picture. Existing
1080p-only fpvOS firmware continues to expose only 1080p in this menu.

720p requires fpvOS's `0021-drm-artosyn-ascent-display-modes.patch`, the subsequent
Ascent PLL divider and 720p lane-rate corrections, and the `ascent-60hz` board
profile. The additional rates require the subsequent HDMI refresh-rate
patches and the `ascent-hdmi` VO/DSI profile with its matching LT9611 quirk.
720p50/60 use matching DSI PHY timing and a lower shared transport clock; retaining the 1080p transport rate caused vertical duplication across
both the UI and video after resolution changes. Live 720p video and native UI
have been checked
on an HDMI monitor and capture device. The driver initializes the scaler
filter before scanout and requests a modeset when the 720p primary source
changes geometry or format. After the native-size startup frame, the presenter
keeps blank frames at the last decoded source size. Entering/leaving playback,
decoder flushes and video timeouts therefore keep the same scaler configuration
and HDMI signal for equal-size sources. Blank buffers are allocated and filled
once; no software video scaling or periodic idle rendering is needed. The old
decoder buffer is released only after the blank frame's page flip completes.
A real source-size change still allows the driver's required modeset. Other
monitors still need testing, including recovery after output-resolution changes.
The compiled asset profiles and menu do not establish a sustained 60 FPS UI.

A failed live decoder is recreated without restarting HDMI, RF reception or
recording. Recovery replays cached HEVC parameters and requests a new keyframe
through the existing radio path. A second failure before any decoded picture,
or a device-open failure, remains terminal. Only display failures trigger screen
mode rollback/fallback; a decoder or radio failure must not select another HDMI
resolution.

## Run

Stop any other DRM/radio clients first. A display-only check needs no peer:

```sh
kestrel-gnd --assets ./assets --no-radio --show-menu --seconds 20
```

The example exits after twenty seconds and disables scanout. Omit `--seconds`
to keep a manually launched menu running. Firmware deployment below starts it automatically.

Use `--force-display` for development with a disconnected/misreported sink or
a known-compatible mode missing from EDID. Always verify actual visible output;
page-flip completion alone is insufficient.

For reception, pass the air unit's four-byte radio MAC with `--peer`. This is a
session setting and does not modify a factory pairing file. The receiver's own
uplink runs a fixed 14 dBm (25 mW), and it asks for a 1080p60 stream; the air
unit takes the TX Power chosen in the menu. Optional `--control PATH` provides a
local UI socket, `--input DEVICE` reads Linux evdev buttons, and `SIGUSR1` toggles
the menu. Send keys using a second invocation with `--control PATH --send-key
enter` (or menu/up/down/left/right/back/quit). `--preferences FILE` persists visual
preferences; without it they last for the session. `--vector` selects Vector.
`--replay FILE` plays a recorded framed radio stream without opening the radio.
`--screenshot FILE` saves the final ARGB HUD as PAM, including transparency.

## UI reference and verification

The software UI follows the current VRX Pro implementation in
`hud/hud_menu.cpp`, `hud/osd_menu_rows.cpp`, `hud/hud_canopy.cpp` and
`hud/osd_images.cpp`. The [fpvOS simulator](https://gehee.github.io/fpvOS/#try)
is a useful visual reference; the receiver sources define the behavior.

Ground Unit contains HUD, DISPLAY, DVR and SYSTEM. Air Unit contains VIDEO
(only while linked), RADIO, SERIAL and INFO. Group headings are skipped during
navigation. OK opens/edits/applies; Back folds an edit while retaining its
pending preview, folds the panel, then closes the menu. A pending value has
an asterisk and an explanation in the help area. Applying one row does not
persist other unfinished previews.

Theme, HUD visibility and Canopy per-cell/pack voltage are local editable
settings. Unsupported controls are dimmed and cannot execute an action.
Values reflect available backend data; the menu does not invent air-unit
identity, recording space or radio settings. SYSTEM receives build/version,
decoder and display information from the composition root.

The idle still, two-color wordmark, picker labels, panel bounds and animation,
selection ring, expanded values and wrapped help use the original geometry.
The video-acquisition caption is currently simpler than the original signal-lock
animation. Idle video/warp, gallery/statistics and the channel page are not yet
ported. Those require further UI and backend work; this is not full VRX Pro
feature parity.

For repeatable visual review, the host-built tool writes 1080p PAM screenshots
with alpha, without opening hardware:

```sh
.local/build-ascent-host/hud/lvgl/kestrel-ui-shot .local/assets .local/ui-shots
```

Compare these with the original `hud_shot` tool using the same Chakra Petch font
and background. Render tests also compare incremental frames against full redraws
during opening and closing, and check that passive counters refresh at a bounded
rate while navigation remains immediate.

## Dynamic HUD movement

At 720p and 1080p, Dynamic HUD moves the cached panels horizontally and vertically
while keeping them level. OFF centers them; SMALL, MEDIUM and EXTREME retain the
existing live movement strengths. Opening a menu centers the HUD. Playback uses
recorded offsets with the same translation-only rendering, and pause holds the
recorded pose. There is no synthetic movement when telemetry is unavailable.

Movement reuses the existing ARGB overlay buffers and invalidates the old and
new panel bounds. Video decoding and hardware video/UI composition remain
independent. No GDC driver or additional DMA surfaces are required. Rotation
telemetry is still recorded, but the native LVGL frontend does not apply it.

With live video, the translation benchmark measured about 58 distinct HUD
updates/s at 720p with a 60 Hz loop, and about 30 updates/s at 1080p. These rates
are distinct from HDMI refresh and vary with content and movement. A stationary
HUD is redrawn only when its content changes.

## Hardware composition check

Stop the receiver before running:

```sh
ascent-planes-probe /path/to/1080p-annex-b.hevc
```

The probe feeds a repeatable HEVC clip to Wave5 at 60 pictures/s, uploads one
ARGB test surface, then sleeps its UI producer for ten seconds while counting
scanout updates. Use HDMI capture to confirm video motion and alpha behavior;
DRM events alone do not establish visible correctness. The probe releases both
planes and decoder resources afterward. Its count is video presentation rate,
not the frame rate of a stationary UI or the capture card.

`--profile` on the receiver reports UI-loop cadence, redraws and HDMI updates
separately. Rendering is included in the configured frame budget rather than
followed by an unconditional extra sleep.

For frame timing, create a writable diagnostic directory and pass
`--trace-dir DIR`. Writing `20` to `DIR/ltrace.req` records twenty seconds of
radio arrival, decoder submission/completion and DRM flip events in memory,
then saves `DIR/ltrace.bin`. Copy that file to the development host and run
`python3 tools/ltrace.py FILE`. Capture-to-arrival values are relative to their
minimum because the camera and receiver clocks are not synchronized. Keep
traces in ignored `.local/`; tracing is disabled unless explicitly requested.

## Current limits

This is an initial bring-up target, not a flight-ready receiver.

- Fixed 1920x1080 I420 video, with 1080p50/60 or 720p50/60/100/120 HDMI. Output mode
  changes restart the receiver session; other refresh rates are not supported.
- Hardware scanout sustained 600 video updates over a ten-second interval with
  the UI uploaded exactly once. HDMI capture confirmed moving decoded video,
  opaque red/white patches, half-transparent green and fully transparent pixels.
  This verifies the video/display path at 60 updates/s; LVGL animation rendering
  is separate and is not yet demonstrated at 60 FPS.
- The LVGL menu, control socket, hardware decoding, buffer recycling and orderly
  shutdown have been exercised. Visible menu output is confirmed on a directly
  connected HDMI monitor and through a USB HDMI capture card. The recreated
  idle artwork, menus, pending theme preview, preference saving and menu folding
  have now been exercised on the receiver. Capture initially showed black frames
  or a vertical offset; cycling capture-card power restored a correctly aligned
  image. Capture re-lock behavior still needs further investigation.
- Short live RF runs now work with the burst-tolerant decoder queue. A
  multi-minute 5 MHz run decoded over 7,500 frames without decoder drops, queue
  overflows or capture errors, including bursts of 18 pending pictures. Long
  keyframe-recovery freezes were eliminated in this test; brief stutters remain
  with periodic transmitter keyframes. With P1's gradual-refresh encoder and
  fpvOS's Wave5 refresh control, a subsequent 20 MHz one-minute trace delivered
  3,600 pictures with no complete-frame arrival gap above 28 ms and no HDMI
  flip gap above 34 ms. Reconnecting requested an IDR and resumed video.
  Narrow-band validation of gradual refresh and long-duration RF reliability
  remain outstanding.
- Native menus expose supported RF/camera controls and stream DVR/gallery.
  FC readings require a valid MSP feed; unavailable capabilities stay disabled.
  Physical buttons and asynchronous beeper feedback are wired into Kestrel; LINK queues the
  existing radio binding command, whose native end-to-end behavior remains
  unvalidated. See [button and beeper interfaces](ascent-input-feedback.md).
  Unavailable temperature and supply readings display `--`.
- Native SDIO does not run the USB firmware's frame-clock register sampler or
  automatically restart the radio driver after errors.

The native Canopy carries the VRX Pro voltage/current, CELL/SAG and RC/VIDEO
tracks, flight mode/timer, calibrated distance, standby label, VTX frequency,
bandwidth, power and temperature, and received video/link bitrate and mode.
Frame rate is measured at scanout; the latency figure is the median of the
past second's capture-to-scanout measurements, available only when the air
clock is known. Missing or expired readings display `--`. The current Ascent
firmware exposes no validated local temperature/supply provider; P1 also
reports its CPU temperature as unavailable. Neither is replaced with a guessed
value. The native whole-frame decoder has no per-slice latency measurements.

Recording has a red square and elapsed `REC m:ss` badge above the left wing,
independent of the details setting. With Canopy hidden, the badge moves to the
top strip, including when the clock is off. Its state follows the recording
worker, so an unsuccessful start does not announce an active recording.

HUD > Link Edge Warning defaults on. It follows the VRX Pro policy: MCS 1–2
is red, 5–7 amber, and 8+ clear; a gap over 400 ms after video has started on
the current connection also turns it red. Worsening is immediate; recovery
must hold for two seconds. Top and bottom glows fade in over 150 ms and out
over 600 ms; red pulses. Cached native-size alpha strips keep this animation
independent of HUD text rasterization. A settled amber warning does not need
continuous UI redraws. Paused gallery playback does not count as stalled live
video.

Keep experiment logs, captures, local configurations and builds in ignored
`.local/`. Use the packaged runtime and fpvOS service for automatic startup; stop that service before manually launching a diagnostic receiver.


## Firmware deployment and power-on

Build the native static executables and both UI profiles, then package them:

```sh
python3 tools/package_ascent.py .local/build-ascent .local/runtime.squashfs
```

The host needs `squashfs-tools` and the matching AArch64 `strip` tool (override
with `--strip`). The packer takes only executables and generated UI profiles,
never preferences, pairing, recordings, or development files. It losslessly
encodes idle themes relative to their shared background (KUIB v2). The receiver
restores one image only when its theme changes; there is no per-frame decoding.
Both KUIB v1 build assets and v2 packaged assets remain supported.

In fpvOS, build `ascentvrx_defconfig` with `BR2_PACKAGE_KESTREL_NATIVE=y` and pass
`KESTREL_NATIVE_IMAGE` as the absolute path to that SquashFS on every build.
After changing the runtime, use `kestrel-native-reinstall` before building the
firmware again. Install the resulting firmware through fpvOS's board-aware USB
updater. Its existing kernel, root image, and bundle size checks still apply.

Normal boot mounts `/usr/share/kestrel/runtime.squashfs` at `/opt/kestrel`, starts
the native decoder/radio and the supervised application, and restores the saved
HDMI mode, preferences and peer. An opaque bootstrap surface covers the hardware reset sequence. At 720p,
the presenter first initializes the native raster, then establishes a scaled
1080p black video surface before publishing UI or live video. The first 1080p
video frame therefore does not reset the HDMI link. No application files or host launch commands
are needed on the SD card. An unpaired receiver starts with the idle screen and
binding controls; `--peer` remains a session-only diagnostic override.

Writable SD state lives in `/mnt/sd/fpvos/kestrel`: `ui.conf`, `receiver.conf`,
`peer`, and `ssh/`. `/var/lib/kestrel` points there. Recordings and their existing
thumbnail/telemetry sidecars live under `/mnt/sd/fpvos/recordings`. For an existing
manual installation, stop its app, copy its visual/receiver preferences, migrate
its peer to a four-byte colon-separated line in `peer`, and move or symlink the
recording folder before restarting. These are device-local data, never firmware
inputs. No vendor partition is formatted or reused for settings.

Without writable SD storage, the application and full UI still run from flash.
Settings/pairing are temporary in `/run/kestrel-state`, and recording is disabled
by an unavailable recording path. Inserting a card requires remounting storage
and restarting the service (or rebooting); automatic card hotplug is not part of
this deployment. Missing HDMI is retried by the supervisor, so connecting a
monitor later does not require a host command.

`/etc/init.d/S60kestrel-gnd {start|stop|restart|status}` owns the application.
Its supervisor bounds RAM logs to two 1 MiB files in `/run/kestrel-gnd`, retries
failures with 2–10 second backoff, and reaps the HUD helper before restarting.
Normal shutdown stops the app before unmounting SD. The control socket is
`/run/kestrel-gnd/control`. Sending `quit` under supervision restarts the app;
use the service's `stop` operation for maintenance.

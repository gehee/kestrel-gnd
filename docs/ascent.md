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

Compressed input absorbs short SDIO read bursts (up to 16 pending pictures),
with an 8 MiB byte budget and a 150 ms pending-age limit. Overflow or decoder
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

## Run

Stop any other DRM/radio clients first. A display-only check needs no peer:

```sh
kestrel-gnd --assets ./assets --no-radio --show-menu --seconds 20
```

The example exits after twenty seconds and disables scanout. Omit `--seconds`
to keep the menu running; this target does not start automatically at boot.

Use `--force-display` only when the connected display reports disconnected and
requires fpvOS's fixed-mode workaround. Always verify actual visible output;
page-flip completion alone is insufficient.

For reception, pass the air unit's four-byte radio MAC with `--peer`. This is a
session setting and does not modify a factory pairing file. The receiver starts
at fixed 25 mW with a 1080p60 stream request. Optional `--control PATH` provides a
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

## Current limits

This is an initial bring-up target, not a flight-ready receiver.

- Fixed 1920x1080 I420 video and 1080p60 HDMI; resolution changes stop the decoder.
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
- Short live RF runs now work with the burst-tolerant decoder queue: one run
  decoded 4,653 frames with no decoder drops, queue overflows or capture errors.
  Earlier firmware tests also reproduced RF drops/damage with the reference
  radio tool. Long-duration RF reliability remains unvalidated.
- Menu edits currently cover visual preferences. RF/camera configuration,
  DVR/gallery and FC telemetry integration remain incomplete. Physical buttons
  and asynchronous beeper feedback are wired into Kestrel; LINK queues the
  existing radio binding command, whose native end-to-end behavior remains
  unvalidated. See [button and beeper interfaces](ascent-input-feedback.md).
  Unavailable temperature and supply readings remain blank.
- Native SDIO does not run the USB firmware's frame-clock register sampler or
  automatically restart the radio driver after errors.

Keep experiment logs, captures, local configurations and builds in ignored
`.local/`. Do not install this bring-up executable as an automatic boot service.

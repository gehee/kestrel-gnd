# Native Ascent VRX

The initial Ascent target runs on native AR9311 fpvOS. It reuses the LVGL
Canopy/Vector frontend from the development branch and the current radio model,
Annex-B assembly, and air-unit protocol. It does not depend on the stock media
service, Rockchip MPP, FFmpeg, Cairo, EGL or GLES.

The composition root is `app/ascent_main.cpp`. `platform/ascent.cpp` supplies a
V4L2 decoder and a single-plane DRM presenter. The HUD renders to an offscreen
ARGB surface; the presenter combines it with decoded I420 in a separate scanout
pool, preserving decoder ownership. Completed frames are presented; partial-slice
scanout is not supported. The presenter drops superseded pictures and blanks
video after two seconds without a decoded frame.

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

## Current limits

This is an initial bring-up target, not a flight-ready receiver.

- Fixed 1920x1080 I420 video and 1080p60 HDMI; resolution changes stop the decoder.
- The current MMAP/copy presenter is substantially slower than decoding on AR9311.
  It cannot currently sustain 1080p60 presentation. A faster buffer/composition
  path remains necessary.
- The LVGL menu, control socket, hardware decoding, buffer recycling and orderly
  shutdown have been exercised. Visible menu output is confirmed on a directly
  connected HDMI monitor. Capture-card output remains unvalidated.
- Live RF association works, but repeated link drops and damaged video were also
  reproduced with the fpvOS reference radio tool. Continuous live video remains
  unvalidated.
- Menu edits currently cover visual preferences. RF/camera configuration, binding,
  DVR/gallery, FC telemetry integration, and physical board-button mapping are
  still pending. Unavailable temperature and supply readings remain blank.
- Native SDIO does not run the USB firmware's frame-clock register sampler or
  automatically restart the radio driver after errors.

Keep experiment logs, captures, local configurations and builds in ignored
`.local/`. Do not install this bring-up executable as an automatic boot service.

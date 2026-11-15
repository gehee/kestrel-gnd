# P1 goggles native UI

Use the [native AR9311 build](ascent.md#build) with
`-DKESTREL_HAL=ar9311 -DKESTREL_BOARD=p1goggle`. Package the resulting build
with `tools/package_ascent.py BUILD_DIR runtime.squashfs`; fpvOS consumes it
through `KESTREL_NATIVE_IMAGE` in its `p1-goggle` profile.

The board reuses the native LVGL frontend, Wave5 decoder and atomic DRM
presenter. Linux selects the internal DSI panel and its fixed 1920×1080 mode.
The native controls factory selects the `p1-goggle-keys` evdev device by
name; it never opens Ascent keypad or beeper devices. The board device tree
maps its ADC ladder to standard navigation, record and connect keys.
The shared debouncer requires stable idle, filters transitions, and repeats
only directional keys. Enter opens/selects menus, Back exits, Record invokes
the existing recording action, and a short Bind press requests pairing when
a live radio is running.
Generic evdev and the control socket remain available.

For offline UI bring-up, use `--no-radio --show-menu` and the packaged assets.
Offline mode opens the decoder only when local playback is requested, so
menus can run without decoder firmware. Live-radio mode retains its existing
startup behavior. fpvOS owns backlight setup and service supervision.

Physical UI input has been confirmed on the goggles. With the P1-specific
Open8030 profile and host-merged factory calibration, fpvOS has also received
and decoded live video from a P1 Air. Radio loss counters were nonzero.
A recorded per-key regression, bind-button pairing, persistent settings and
longer RF stability tests remain follow-up work. Do not substitute Ascent
firmware or board calibration merely because the SoC matches.

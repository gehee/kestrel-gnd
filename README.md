# kestrel-gnd

The ground application of **fpvOS** — what you actually look at through the
goggles. It decodes the link, draws the HUD and the menus, and records.
C++ on DRM/KMS with FFmpeg or Rockchip MPP, rendering straight to the display
controller: no X11, no Wayland, no compositor.

Built and packaged by [fpvOS](https://github.com/gehee/fpvOS), which supplies the toolchain, the
kernel and the root filesystem. This repository is only the application.

```
kestrel-gnd ──libar8030_client.so──► TCP 127.0.0.1:50000 ──► /ar8030soc/daemon
                                                                   │ usbfs
                                                             AR8030 baseband
                                                                   │ RF
                                                                air unit
```

Baseband sockets, slot 0: port 3 video, port 2 control + telemetry (`FE A5`
sky protocol, MSP teed off the same stream).

---

## Features

**Video** — H.265 and H.264, hardware decode through RK MPP on the device
(VAAPI or FFmpeg elsewhere), DRM/KMS output straight to a plane. Variable
refresh rate unless `--disable-vrr`. Screen mode overridable with
`--screen-mode 1920x1080@120`.

**OSD** — a separate DRM plane, so `--no-osd` leaves the video path untouched.
Five menu tabs: VIDEO, RF LINK, HUD, PHYSICS, SYSTEM. Latency panel, link stats (SNR, MCS, frequency, bandwidth, gain), TX power,
LDPC error ratio, FPS/bitrate, and a Betaflight MSP grid fed from the telemetry
socket.

**Radio control** — channel, bandwidth, MCS, TX power, LNA mode and frequency,
plus camera settings pushed to the air unit over the `FE A5` sky protocol.

**DVR** — recording is on the REC button; files land in `dvr_dir`
(default `/media/dvr`). `--dvr-format raw|mp4|fmp4`, `--dvr-framerate N`.
Screen recording rebuilds each frame from what the display scanned out - the
decoded picture and the OSD, blended with the RK3568's RGA - and encodes it on
the VEPU at 60 fps, without touching the display, so the live picture loses
nothing to it (`screen_tap.hpp`). It needs RKMPP and librga; without them there
is no screen recording, and the log says so.

**Settings** — `kestrel-gnd.yaml` next to the binary.

---

## Running

```sh
kestrel-gnd --link ar8030 --osd
```

`--link ar8030` is the only supported source: video arrives on baseband slot 0
port 3, control and telemetry on port 2. `kestrel-gnd --help` lists the rest,
including the `--ar8030-*` block for frequency, power, bandwidth and camera mode.

## Binding an air unit

Three ways in, all the same code path: the goggle's **BIND button**, the
**Bind** row at the top of the RF LINK menu, or `--ar8030-pair` at startup. A
banner reports `BINDING...` with a countdown, then `BOUND` with the address it
found, or `BIND FAILED`.

**Both ends have to be in pairing mode at the same time.** This is the whole
trick, and it is easy to lose hours to. The air unit is the access point and
the goggle only listens; a goggle listening alone runs its full window and
reports nothing, however healthy everything else is.

On a Caddx Ascent air unit, pairing mode is **the RED LED**. Hold its bind
button until the LED turns red, then press bind on the goggle. Neither the
blinking green at boot nor the solid green after a short press is pairing
mode, and nothing in the air unit's Linux userland contains pairing code at
all - the button is handled by the radio module itself. The goggle gives you
a two minute window, so the order is forgiving; a second press cancels it.

If a window ends with nothing, the log says which of the two failures it was:
`bind sees slot0 <mac>` means the radio heard an air unit, while no sighting
line at all means it heard silence and the air unit was never in pairing
mode.

Binding does not need to be undone first, and a goggle that already has a
working link will still bind a different air unit. Note also that a paired air
unit re-associates on its own when powered, with no candidate list on the
ground at all, so "it still connects" is not evidence that a bind took.

The result is written to `/factory/user_cfg.json` as a 100-entry ring,
`bb_mac_addr_0`..`bb_mac_addr_99` with `save_candidate_position` pointing at the
next slot, matching the stock format exactly. An address already in the ring is
left alone rather than rewritten. All of them are pushed to the radio as the
candidate list at startup, and `--ar8030-ap-index N` picks which one to
associate with when several are known.

## Source layout

| Folder | What is in it |
|---|---|
| `core/` | `main`, settings, the phone view's web server, the WiFi access point, time zones and their channel rules |
| `radio/` | the AR8030 radio: the video and control link, the air unit's messages, the bind |
| `display/` | KMS (`drm`, and the C helpers in `kms/`), the renderer that puts pictures up, the refresh lock, the screen tap |
| `hud/` | the HUD, menu and gallery (`osd*`, `hud_*`, `gallery*`), their state snapshot and the menu's actions |
| `render/` | the canvas the screens draw with, and the GL context |
| `dvr/` | recording, the recordings library, repairing a recording |
| `msp/` | the flight controller's MSP OSD and telemetry, Betaflight's glyphs |
| `hal/` | the hardware interfaces (the decoder in `vdec.hpp`) and their backends: `hal/rockchip/` for the VRX Pro, `hal/ffmpeg/` for the software decoder |
| `bsp/` | one file of facts per box (`vrx_pro.cpp`) |
| `motion/` | the air unit's IMU samples, carried in the video stream, and quaternions |
| `utils/`, `queue/`, `icons/` | helpers, the message queue, icons |
| `splash/` | the boot splash program |
| `tools/test/` | unit tests (`-DKESTREL_TESTS=ON`); `tools/vdec_test.cpp` is a by-hand decoder test, not built |

## Building

```sh
cmake -S . -B build -DKESTREL_HAL=rockchip && cmake --build build -j
```

### Build options

`KESTREL_HAL` picks the set of hardware backends: `rockchip` for the VRX Pro
(MPP decoder, RGA, the VOP2 display lock), or `pc` for a host build with the
ffmpeg decoder. `-DUSE_RKMPP=ON`, which older fpvOS releases pass, still
means `rockchip`.

Features are switched at build time, so a box that can't run one doesn't
carry its code. fpvOS sets them from its Kconfig options, one set per board:

| Option | Default | What it adds |
|---|---|---|
| `KESTREL_GLES` | on | draws the HUD, menu and gallery with GLES; required until there is a CPU canvas |
| `KESTREL_STREAM_DECODE` | on with `rockchip` | per-slice decode, early presentation and the per-half presenter |
| `KESTREL_SCREEN_ENC` | on with `rockchip` | screen recording and the phone view's encoder |
| `KESTREL_WIFI` | on | the WiFi access point (off: the menu says the system has none) |

The choices are in the generated `kestrel_gnd_features.h`, every one defined
to 0 or 1.

### Tests

```sh
cmake -S . -B build -DKESTREL_TESTS=ON && cmake --build build -j && ctest --test-dir build
```

The unit tests are in `tools/test`, one program each, exiting non-zero on a
failure. Built with the device toolchain they run on the unit as they are:
copy them over and run each one. None of them touches the hardware.

### Cross-compiling for the VRX Pro

The toolchain and sysroot live in fpvOS, not here. Build through it, and it
will fetch this repository at the revision fpvOS pins - a **release tag** once
there is a release, and a commit until then:

```sh
make -C buildroot BR2_EXTERNAL=$PWD/br-external kestrel-rebuild
```

While working on kestrel you do not want that fetch. Point the package at
your checkout and Buildroot compiles your working tree instead, skipping the
download entirely:

```sh
make -C buildroot BR2_EXTERNAL=$PWD/br-external KESTREL_OVERRIDE_SRCDIR=/path/to/kestrel
```

Work lands on `main`, and fpvOS pins the commit it has been built and tested
with. A release is cut by `scripts/release.sh` in fpvOS, which tags that
pinned commit here and pins the tag - so both repositories carry the same
version number and an fpvOS release builds to the same thing forever, offline
included. There is no release branch; the tag is the release. To build a
branch's current head instead of the pin, without a local checkout, pass
`KESTREL_BRANCH=main` to make.

`KESTREL_HAL=rockchip` is required for the device: it pulls in the Rockchip
decoder and the hardware encoder used by screen recording. Linking also needs
`libar8030_client.so`, the radio client library. fpvOS builds it from
ar_libre's `lib/`; for a host build, build that library with the host
compiler and pass it as `-DAR8030_LIB=/path/to/libar8030_client.so`.

## Third-party code

kestrel-gnd is GPLv3 ([LICENSE](LICENSE)). It carries two pieces under their
own licences, unchanged:

- `utils/qrcodegen.c`, `utils/qrcodegen.h`: the QR Code generator library by
  Project Nayuki (<https://www.nayuki.io/page/qr-code-generator-library>),
  MIT License. It draws the QR codes on SYSTEM › WiFi AP that join a phone to
  the goggle's network and open the spectator page. The licence text is at
  the top of both files.
- `include/rga/`: the im2d API headers of Rockchip's librga, Apache License
  2.0 (see [include/rga/README.md](include/rga/README.md)).

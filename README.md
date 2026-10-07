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
nothing to it (`screen_tap.hpp`). It needs RKMPP and librga; without librga it
falls back to the DRM writeback connector, which costs the live picture.

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

## Building

```sh
cmake -S . -B build -DUSE_RKMPP=ON && cmake --build build -j
```

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

`USE_RKMPP=ON` is required for the device: it pulls in the Rockchip decoder
and the hardware encoder used by screen recording. Linking also needs
`libar8030_client.so`, the proprietary AR8030 client, which is extracted from
firmware you own rather than shipped by fpvOS — the `fpvos-vendor-libs`
package stages it. CMake tolerates its absence, so a host build without
vendor blobs still compiles; `--link ar8030` then has nothing to talk to.

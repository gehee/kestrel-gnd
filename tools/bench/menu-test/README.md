# VRX Pro menu test

A bench test that walks every setting of the VRX Pro's menu on a real goggle
with an air unit linked, and checks what each one did. Run it before a
release, and after any change to the menu, the radio settings or the HUD.

It drives the goggle's real input path: `keysd` is a uinput keyboard on the
goggle, and `menu_test.py` sends it keys over ssh and reads kestrel-gnd's log
(`menu: TAB > Row: value (edit|live|enter)` and each row's own lines).

## What it checks

| Part | How |
|---|---|
| Every setting row of HUD, DISPLAY, DVR, SYSTEM, VIDEO, RADIO | each value applied in turn with Enter; the row's log line seen; kestrel-gnd still running (same pid); video still flowing (a 5 s decoder count of 100+ pictures); then the row's first value put back |
| Rows with their own log lines | Frame Mode, Sync Margin, TX Power / TX Power Mode: that line too |
| Refresh Sync | OFF, NUDGE, FOLLOW: each trial seen and kept, back to the first value |
| Screen Mode | each other mode on trial (kestrel-gnd restarts into it), video seen, turned down (restart back) |
| VRX Radio Firmware | switched (radio and kestrel-gnd restart), switched back from the unlinked RADIO tab, link and video back |
| RADIO > Channel | the page opened, the link moved one channel away and back, video after each |
| INFO, SERIAL | walked (read-only rows) |
| Settings | the settings file at the end compared with the one at the start (measured values left out) |

Not exercised, on purpose: **Bind** (it pairs; `KEY_VOLUMEUP` binds even with
the menu closed and is never sent), **Format DVR** (erases the card), the
gallery's delete (Esc on the live screen is never sent), Tab.

## Running it

Needs: the VRX Pro on USB (`root`/`fpvos` at 192.168.3.1), an air unit linked
with video, `sshpass` on the PC, and `keysd` built for the goggle:

```sh
<output>/host/bin/aarch64-buildroot-linux-gnu-gcc -O2 -Wall -Wextra -Werror \
    -o tools/bench/menu-test/keysd tools/bench/menu-test/keysd.c
python3 tools/bench/menu-test/menu_test.py --report menu-test.md
```

- `--only HUD,DISPLAY` walks only those tabs (also `CHANNEL`; the radio
  firmware switch is part of `RADIO`).
- `--max-values N` stops a row after N values (a quick pass).
- `--skip-restarts` leaves out Screen Mode trials and the firmware switch.

It starts kestrel-gnd again first (the menu in a known state), copies the
settings file to `/etc/kestrel/kestrel-gnd.yaml.pre-menutest-run`, and exits
non-zero on any failed check or changed setting. A full run takes about an
hour, most of it waiting for video after each change.

If a run stops half way, the settings file may hold a test value: stop
kestrel-gnd (`/etc/init.d/S70kestrel stop`), copy the `.pre-menutest-run` file
back, and start it again.

## Limits

- Rows are found by probing (Enter on a setting only logs `(edit)`), so the
  test follows whatever the menu lists on this goggle with this air unit:
  rows hidden for this combination (Strips and Max Exposure without a P1,
  Max MCS on stock radio firmware) are not walked. Run it with each air unit.
- It checks behaviour through the log, not the picture: layout, colours and
  text on screen still need a look (`kill -USR1 $(pidof kestrel-gnd)` records
  the screen).
- The native AR9311 receivers have their own menu and their own test, below.

## Native receivers (SteadyDigital, Ascent VRX, P1 goggles)

`native_menu_test.py` walks the LVGL menu (`hud/lvgl`) through kestrel-gnd's
control socket (`--control /run/kestrel-gnd/control --send-key KEY`), whose
reply is the menu state, so no `keysd` is needed. The native menu logs no row
changes: each change is read from `/var/lib/kestrel/ui.conf` and
`receiver.conf`, and from the radio's own log lines.

```sh
python3 tools/bench/menu-test/native_menu_test.py --iface enx029311000202 --report native.md
```

- `--iface` picks the USB interface when two units share 192.168.3.x
  (`devproxy.py` binds the ssh connection to it); the SteadyDigital is
  `root`/`root` at 192.168.3.104.
- `--only`, `--skip-restarts`, `--report` as above.
- A setting not saved yet at the start (its default) is written when the walk
  comes back to it; the walk then steps Up once onto the value it started on.
- The channel page: from AUTO it moves to a channel and back to AUTO (a new
  channel search), from a fixed channel to the one below and back.
- Both settings files are copied to `*.pre-menutest` first and put back at
  the end (kestrel-gnd restarted) if anything differs.

Not exercised: Bind, Record, Gallery, Format SD Card, Back on the live screen.

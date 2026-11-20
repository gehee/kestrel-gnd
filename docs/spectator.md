# Spectator mode

Native AR9311 receivers (Ascent VRX and P1 goggles) offer **RADIO > Receiver
Mode > Pilot / Spectator**. Pilot remains the default. Spectator receives an
existing pilot/air-unit video stream without associating, acknowledging packets,
or sending air-unit commands. The pilot retains normal association; periodic late-join recovery requires a supporting air build.

## Setup

1. Keep a normal pilot receiver paired with the air unit. Read its current
   frequency, bandwidth and stream ID.
2. On the second receiver, select **Spectator** under **Receiver Mode**, then set
   **Listen Frequency**, **Listen Bandwidth**, and **Stream ID** on RADIO.
3. A pilot running this Kestrel version shows **Spectator Info** under RADIO, with frequency, bandwidth and stream ID.
   Otherwise, Stream ID is the XOR of the four bytes of the air unit's radio MAC. It is an
   eight-bit filter, not authentication; unrelated air units can share an ID.
4. Wait for the air unit's periodic recovery picture. The spectator never
   requests one itself. Older air builds without periodic recovery may need
   a pilot reconnect.
5. Select **Pilot** to return to normal pairing. Existing pairing is retained.

Mode and tuning use the normal receiver settings file and survive application
restarts. Bind, channel scan, camera, TX power and other air-unit controls are
disabled while listening. Receiver-local display and recording controls remain
available. Spectator Status distinguishes waiting, received data, and an
unavailable listener. Video display still requires decodable frames.

Initial tuning is manual. With LST2 firmware, the receiver follows subsequent
AP channel and 5/10/20/40 MHz bandwidth controls without transmitting. Link
SNR, ranging, uplink power and normal association telemetry are not available.

## Implementation and compatibility

Use an Open8030 build implementing LST1 and the read-only capability request
`0x0c5a0037`. An older firmware is refused before listener configuration; there
is no automatic fallback to transmitting pilot mode. ar_libre's existing
bounded request and socket APIs are sufficient; no modified library is needed.
The wire adapter lives in `radio/spectator_protocol.hpp`.

Kestrel reads host video socket 3 directly into the existing decoder and DVR
pipeline. There is no FIFO, external helper or file-replay pacing. A distinct
radio lifecycle opens no control/MSP socket or normal stats thread. It stops a
previous session before listening, rearms the same channel after an idle
interval, and periodically checks hardware TX hold and the firmware's DEV,
TX, preencode and ACK counters. A failed check stops the session. An explicit
return to Pilot restores normal initialization from Open8030's saved loader
configuration.

The LVGL native receiver frontend exposes these controls. The legacy VRX Pro
frontend has not been integrated or hardware-validated for spectator mode.

Tests cover protocol encoding/rejection, saved settings, menu gating, model
commands, and firmware receive-only behavior and explicit pilot restoration.
Register/counter checks do not replace an independent RF emission measurement.

## Sharing and late joining

The pilot exposes read-only current frequency, bandwidth and stream ID in RADIO → Spectator Info, directly below Receiver Mode. There
is no pilot spectator toggle. Kestrel Air defaults to periodic recovery pictures
and small stream announcements, allowing late joining after manual RF tuning.
The air's existing saved override remains respected.

Use Open8030 spectator capability API revision 2 (LST2). Initial frequency and
bandwidth are saved settings; current tuning is read back from the radio and
shown separately. A receive stall rearms the most recently followed tuning.
The loader's channel catalog must have the same ordering as the air unit's.

Followed controls cannot enable association, ACKs or uplink. Channel/width
transitions can lose pictures; this is not a seamless-handover guarantee. A
missed announcement or an unannounced air restart can require manual retuning.
Automatic initial discovery remains outstanding.

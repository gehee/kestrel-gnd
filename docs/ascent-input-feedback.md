# Ascent Gnd485 buttons and beeper

The Gnd485 stock firmware polls an external I²C ADC for the menu keys, a
separate GPIO for LINK, and drives the beeper through PWM. These are different
board connections from the RK3568 VRX Pro and the AR9311 VR04 goggles.

This firmware-derived interface map has been confirmed through physical button
presses and a dedicated audible beeper test on native fpvOS. PWM operation and
register readback have also been tested.
The physical SoC package pad, PCB net and beeper transistor polarity have not
been established. No unidentified GPIO was driven during this investigation.

## Recovered board connections

| Function | Gnd485 interface | Evidence |
| --- | --- | --- |
| Front menu keys | I²C3, controller `0x01206000`, 7-bit address `0x54`, register `0x00` | Stock backend selects `/dev/i2c-3` for board 485 and reads two bytes with `I2C_RDWR` |
| LINK / pairing | GPIO bank A, offset 2; active-low | Startup script explicitly calls GPIOA2 the pair key; backend reads GPIO 2 and treats zero as pressed |
| Beeper | PWM controller `0x01000000`, channel 0 | Stock UI's beeper helper uses `pwmchip0/pwm0`; stock DT enables this controller and disables `0x01002000` |

The stock DT's misleading node name `pwm@08000000` has a **reg address of
`0x01000000`**. Use the reg address to identify the controller. Discover Linux
PWM and GPIO chips by their device identity rather than assuming probe-order
numbers. GPIOA2 means bank A offset 2, not necessarily Linux global GPIO 2 on a
different driver.

GPIO 106 is the HDMI bridge interrupt input in the receiver startup script;
it is not the pairing key. GPIOs 4, 5 and 6 are hardware ID inputs. The VR04
goggle's beeper controller `0x01002000` must not be substituted here.

### Menu key conversion

The stock transaction writes the one-byte register pointer `0x00` followed by
a repeated-start, two-byte read from address `0x54`. It combines the reply as
big-endian, then uses:

```text
code = (word >> 4) & 0xff
millivolts = trunc(code * 3300 / 256)
```

The nominal Gnd485 ladder centres and observed physical presses are:

| Key | Nominal mV | Observed mV |
| --- | ---: | ---: |
| REC | 400 | 386 |
| UP | 800 | 786 |
| LEFT | 1000 | 992 |
| RIGHT | 1200 | 1185–1198 |
| OK | 1400 | 1392 |
| DOWN | 1600 | 1585–1598 |
| ESC | 1800 | 1778 |

The stock matching window is ±89 mV. Its board-485 branch replaces the generic
ladder's MATCH entry with ESC at 1800 mV and limits the table to seven entries;
LINK is read separately. Do not use the generic table's ESC value of 600 mV.
Stock collects ten usable readings and removes the minimum and maximum before
averaging; it also applies a configured `key_adc_offset`. The observed readings
above use no offset and all fall within the stock windows. Idle reads zero.
A single 464 mV sample occurred while releasing LEFT, inside the REC window;
the input layer must debounce transitions rather than emit every decoded sample.
LINK independently changed from high to low during its press and returned high
on release, with the ADC remaining zero. The external
ADC part number remains unidentified; no SoC SAR ADC channel is implicated by
this code path.

Stock LINK handling distinguishes release before an eight-second hold from an
upgrade request after that hold. This is evidence about the original behavior,
not authorization to start an upgrade from a raw key event in Kestrel.

### Beeper drive

Stock UI callers select channel 0 and a period of **1,000,000 ns (1 kHz)**.
The beeper helper exports the channel if needed, disables it, clears duty
before changing period, sets duty as a percentage, enables it for the requested
duration, then clears duty and disables it again. Examples recovered from the
callers are:

| Caller behavior | Duty | Tone duration |
| --- | ---: | ---: |
| Volume-dependent short feedback | 1–11% depending on setting, with a 5% fallback | 40 ms |
| Fixed short feedback | 20% | 100 ms |
| Duration-parameterized notification | 50% | Argument in milliseconds |

These call sites establish available tone settings; not every caller's product
meaning has been reconstructed. The startup script separately pulses channel 0
at 1 kHz, 10% duty for 5 ms. Under native fpvOS, two 100 ms pulses at 1 kHz,
10% duty, separated by 250 ms, were requested. Hardware readback after release
showed the channel disabled; audible output from that test was not confirmed.

A subsequent dedicated listening test used **1 kHz, 20% duty**, with pulses of
150 ms, 150 ms and 500 ms separated by 350 ms. The operator explicitly confirmed
hearing that exact short-short-long pattern from the VRX. PWM was disabled and
unexported afterward, with hardware readback confirming disabled state. This
confirms audible output for the tested pattern; other stock tone patterns remain
untested. Duty controls electrical drive; it is not a linear calibrated loudness
scale.

## Integration boundary

Follow the VRX Pro division of responsibility, with an Ascent-specific input
transport:

1. **fpvOS** exposes I²C3 and `i2c-dev`, the existing GPIO controller, and the
   Artosyn PWM provider at `0x01000000`. Pinmux and clock setup belong here.
   Keep the second PWM controller disabled unless a separate consumer needs it.
2. **Kestrel BSP** describes the controller identities, ADC transaction and
   conversion, key centres, LINK polarity, and beeper channel. Keep these facts
   out of LVGL and the common controller. The existing BSP's IIO-only ADC path
   cannot represent this external I²C conversion without an adapter.
3. **Kestrel input** reads and debounces the ladder and LINK, repeats direction
   keys only, and emits common key/action events. Invalid/failed ADC reads must
   release the candidate key, never become a press. Acquisition should not
   block rendering. Use the common UI controller directly; the old UDP 6222
   relay was an interface between stock processes, not a hardware requirement.
4. **Kestrel feedback** owns the PWM output through one bounded, asynchronous
   scheduler. Key feedback is low priority; binding and upgrade status can
   replace it. Coalesce repeated key clicks, cancel obsolete patterns, and
   disable PWM on shutdown/error. Report success/failure from actual operation
   state, not from the key that requested the operation.
5. **Upgrade ownership** needs an explicit handoff if the updater outlives
   Kestrel. Kestrel must release the PWM before the updater takes over; two
   processes must not concurrently schedule beeps on the same channel.

fpvOS commit `edd9790` enables I²C3, built-in `i2c-dev` and the Artosyn PWM
provider. The installed image passed update readback, a cold boot, PWM range
and polarity checks, and decoder/platform regression checks. Physical button
press/release and the dedicated audible beeper pattern are confirmed.
Kestrel now acquires the panel in `platform/ascent_panel.cpp`, using facts in
`bsp/ascent_panel.hpp`. The ADC and LINK are sampled on a worker at 50 Hz,
with 40 ms stability and a clean release required between keys. Startup-held
keys, read errors and sampling gaps over 100 ms disarm input until stable idle.
Direction keys repeat after 450 ms, every 140 ms, respecting the UI repeat
preference. A bounded queue drops events older than 250 ms rather than replaying
stale actions after a rendering stall.

The native composition root maps arrows, OK and BACK into the existing LVGL
controller. A short LINK release queues the existing radio binding command
when a live radio is running; a second press uses its cancellation behavior.
Binding notices and tones follow reported radio state. REC and LINK holds of
at least eight seconds show unavailable notices; neither recording nor upgrade
execution is implemented here. Native radio binding still needs an end-to-end
hardware test, including persistence after reboot.

A separate worker owns PWM channel 0, coalesces clicks and lets notifications
interrupt them. Clicks use 60 ms at the verified 1 kHz/20% electrical settings;
binding-start uses two 150 ms pulses, success adds a 500 ms third pulse, and
failure uses one 500 ms pulse, with 350 ms gaps. Direction repeats are silent.
Only the earlier dedicated short-short-long pattern has acoustic confirmation
so far. An already exported PWM is left alone; owned PWM is disabled and
unexported on normal shutdown. Missing providers do not disable other UI inputs.
The ADC also takes an advisory exclusive lock; the GPIO line has kernel ownership.

Host tests cover captured key values, release transients, startup-held input,
read-error recovery, sampling stalls, directional repeats and short/long LINK
release. The native build and all eight host checks pass.

## Reproducible firmware evidence

Reference: extracted official **Ascent G Gnd 17.5.8**, board 485. Only the
recovered interface facts are recorded here; vendor binaries and disassembly
stay in ignored `.local/`.

| Artifact | SHA-256 |
| --- | --- |
| `usrdata/fpv/ar_ldy_gnd` | `5c07452b91a5284b029c03b92636bb6172e04aa2a27a3b62fb832e8729495a0a` |
| `fpv_data/bin/GlassesUI` | `ff7d03aa946239b48a96c2cf880ba6e307d63eef83a3ac64270a4e9f7983b84b` |
| `dtb.bin` | `10021643622ae949d8892152ea0b0ac2e281db5c292eabd7d8b6fa52e120970d` |

Backend symbols/virtual addresses:

- `get_app_param`, `0x424f60`: board 485 maps to project type 2.
- `fpv_key_input_init`, `0x41ca00`: starts `Fxn_key_i2c_detect`, not the older
  SoC ADC polling function also present in the binary.
- `Fxn_key_i2c_detect`, `0x41c3b4`: I²C3 selection for type 2, Gnd485 table
  adjustment, key window and LINK hold handling.
- `i2c_read_2byte`, `0x418c98`: address, register-pointer/read transaction and
  byte order.
- `get_i2c_key_voltage_average_mv`, `0x41c0e0`: conversion and averaging.
- `check_bb_match_gpio_key`, `0x41c2fc`: GPIOA2, pressed at zero.
- Read-only table at `0x5c7e08`: nominal ladder before the Gnd485 adjustment.

The stripped UI's beeper helper is identified by its error strings at
`0x487110`; callers at `0x4844c8`, `0x4845d0`, and `0x484608` select channel,
period, duty and duration. The stock `usrdata/run.sh` independently confirms
the startup PWM pulse and GPIOA2 pairing input.

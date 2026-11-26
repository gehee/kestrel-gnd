# Receiver status LED

Native receivers optionally use the Linux multicolor LED named `fpvos:status`.
The board driver must expose `multi_index` as `red green blue`, accept 0–255
`multi_intensity` components, and use a maximum brightness of 255. No LED is
selected by enumeration: without this named interface there is no worker and
no GPIO, SPI, or LED write. The default applies to receivers without LEDs.

| State | Pattern |
| --- | --- |
| Application starting | Green, one second on / one second off |
| No recent video | Solid red |
| Pairing | Blue, 300 ms on / 300 ms off |
| Recent video | Solid green |
| Recording recent video | Green with a 200 ms blue pulse each second |
| Recording error or unsupported video | Red, 200 ms on / 200 ms off |

Fault takes precedence over pairing; pairing over video status. Video status
uses the UI's recent-frame predicate, not association alone. Lost video remains
red even if a recording is still open. Spectator reception uses the same rules.

`StatusLed::set()` only stores an atomic state. A worker checks it every 100 ms
and writes only on color changes. LED errors disable this optional output and
are logged once; normal shutdown turns the LED off. A forced process kill may
leave the last color until the next start. Firmware-update indication is not
owned by Kestrel and is not implemented here.

The P1 VRX and P1 goggles enable their SPI LED chain through device tree. Other
receiver variants do not inherit that node. The driver mirrors the color to
three physical LEDs using the stock encoding. Application policy does not
contain board-specific GPIO numbers, SPI devices, or encoding.

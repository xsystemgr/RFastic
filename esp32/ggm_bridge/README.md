<!-- SPDX-License-Identifier: MIT -->
# ggm_bridge: GGM2 mesh host for an ESP32 inside the radio

Runs the mesh (`messenger/msg_mesh.c`) and talks to the radio over the messenger link
([docs/bridge-protocol.md](../../docs/bridge-protocol.md)). The phone joins the WiFi access point
`GGM-<CALL>` (password `ggm12345` until you change it) and opens http://192.168.4.1.

## Build and flash

```sh
cd esp32/ggm_bridge
pio run -e xiao_c3 -t upload      # Seeed XIAO ESP32-C3 over its USB-C
pio device monitor                # console: call SV4QXF / send ALL hello / status / wifi off
```

| Env | Board | Radio |
|---|---|---|
| `xiao_c3` | Seeed XIAO ESP32-C3 | UART link (inside the radio) |
| `ttgo` | TTGO/LilyGO with a classic ESP32 (T-Display, LoRa32 v2.x, T-Beam) | UART link, on the K-plug cable |
| `ttgo_oled` | 2017 TTGO ESP32 with a display, CP2104, no LoRa (UART on GPIO26 TX / GPIO25 RX) | UART link |
| `ttgo_sim`, `ttgo_oled_sim`, `xiao_c3_sim` | same boards | none: simulated air over ESP-NOW |

The `ttgo_oled` envs print an I2C scan at boot (console `i2c` repeats it) so the display can be identified.

All envs are compile-checked against arduino-esp32 3.0.7 (-Wall -Wextra, no warnings). None is linked through
PlatformIO or flashed yet.

## Bench test without a radio (`ttgo_sim`)

Flash two or three boards with `ttgo_sim`. Give each its own callsign on the console (`call AAA`, `call BBB`, ...).
They exchange GGM2 frames over ESP-NOW on WiFi channel 1, with the radio's pacing (one frame per 0.9 s per node).
Join any board's access point from the phone and chat.

To test relaying with three boards on one desk, make the end nodes deaf to each other:

```
AAA> deaf CCC        CCC> deaf AAA        (BBB hears both and relays)
AAA> send CCC hello  -> CCC delivers it with "1 hop", AAA's message turns "delivered" when the ACK comes back via BBB
```

`hear CCC` undoes it. `status` shows the relay, suppression and duplicate counters.

## TTGO on the programming cable (`ttgo`)

GPIO33 = TX, GPIO32 = RX, plus GND to the plug. On Baofeng-style K-plugs the data lines are usually the 3.5 mm
sleeve and the 2.5 mm ring, with ground on the 2.5 mm sleeve. Which line is TX differs between cables and radios: if
the radio never answers HELLO, swap TX and RX. The 3.5 mm sleeve also carries mic and PTT on many radios, so measure
before you connect and keep the TTGO's TX idle-high (it is, once the firmware runs). The radio's firmware must have
the messenger link built in.

## Wiring (XIAO ESP32-C3)

| XIAO | Goes to |
|---|---|
| D6 / GPIO21 (TX) | radio MCU UART RX (the programming cable's RX line), through 1 kΩ |
| D7 / GPIO20 (RX) | radio MCU UART TX, through 1 kΩ |
| GND | radio ground, one point |
| 3V3 pad | 3.3 V from your own small buck converter, fed from the battery after the power switch |

Both sides are 3.3 V logic. Measure the radio's UART lines before you solder: the pins differ between radios.

Keep the module and its antenna away from the BK4819 front end. The 40 MHz crystal has harmonics at 160, 440 and
480 MHz. The firmware sets WiFi TX power to 8.5 dBm, and `wifi off` disables WiFi completely; the mesh keeps running
either way.

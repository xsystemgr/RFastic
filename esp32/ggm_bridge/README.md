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

Compile-checked against arduino-esp32 3.0.7 (ESP32-C3, -Wall -Wextra, no warnings). Not yet linked through
PlatformIO or flashed.

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

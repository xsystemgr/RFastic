# GOGU Messenger (GGM2 analog text messenger)

GOGUFW-compatible analog text messenger: it exchanges GGM2 text frames with
UV-K1/K5 radios running GOGUFW, over BK4819 FSK (1.2 kbaud, 100-byte frames).
Extracted from the RadioFoundry fork for reuse in another project.

License: MIT (see SPDX headers in each file).

## Layers (what to take)

1. **Portable core — `messenger/`** (no hardware, no board headers; pure C)
   - `msg_packet.{c,h}` — the 94-byte GGM2 wire packet: build/parse, callsigns,
     message id, CRC. This is the byte-for-byte GOGUFW-compatible format.
   - `msg_fsk.{c,h}` — wraps a packet into the 50 FSK words (0xABCD sync head,
     47 payload words, frame CRC, 0xDCBA tail) and back.
   - `msg_store.{c,h}` — in-RAM conversation store (Sent/Received, dedup, status).
   - `msg_radio.h` — the 4 board hooks the UI calls (you implement these).
   - `tests/` — host tests for the packet format and the store (no hardware).

2. **Hardware FSK modem — `drivers/bk4819_fsk.{c,h}`**
   - Drives a BK4819 into GOGUFW-compatible FSK TX/RX (Tone2 1200 baud, CRC,
     16-byte preamble, incremental FIFO drain). **Depends on your own
     `bk4819.h`** (a `bk4819_t` handle + `bk4819_read/write`). Keep this only if
     your target also uses a BK4819; otherwise reimplement these ~6 functions
     over your own modem.

3. **Example glue — `examples/ui_msgr.c.example`**
   - How the RadioFoundry UI drove the core. Reference only; not required.

4. **Messenger link + mesh — the radio as a modem for an ESP32** ([docs/bridge-protocol.md](bridge-protocol.md))
   - `msg_kiss.{c,h}` — KISS framing (FEND/FESC), streaming decoder.
   - `msg_link.{c,h}` — the link's constants and the host side (encode requests, decode the radio's events).
   - `msg_bridge.{c,h}` — the radio side: parse the host's frames, hand packets to TX, queue RX events and results;
     leaves bridge mode after 15 s of silence.
   - `msg_mesh.{c,h}` — the host's mesh: dedup, slotted relays with suppression, ACKs back over relays, retries.
   - `esp32/ggm_bridge/` — PlatformIO firmware for a Seeed XIAO ESP32-C3: mesh + WiFi web UI + USB console.
   - `patches/radiofoundry-rt880-messenger-link.patch` — the RadioFoundry_QXF change (branch `research/rt880-port`)
     that wires the bridge into the RT-880 messenger (`make -f boards/rt880/Makefile HF=1 MSG=1 LINK=1 image`). Its
     UART driver is still a stub until the programming-cable USART is traced.

## Build & test the portable core (no hardware)

```sh
cc -std=c11 -Wall -Wextra -Werror -I. \
   messenger/msg_packet.c messenger/msg_fsk.c messenger/tests/msg_packet_test.c -o /tmp/msg-packet-test && /tmp/msg-packet-test

cc -std=c11 -Wall -Wextra -Werror -I. \
   messenger/msg_store.c messenger/msg_packet.c messenger/tests/msg_store_test.c -o /tmp/msg-store-test && /tmp/msg-store-test

cc -std=c11 -Wall -Wextra -Werror -I. \
   messenger/msg_kiss.c messenger/msg_link.c messenger/msg_bridge.c messenger/msg_packet.c \
   messenger/tests/msg_link_test.c -o /tmp/msg-link-test && /tmp/msg-link-test

cc -std=c11 -Wall -Wextra -Werror -I. \
   messenger/msg_mesh.c messenger/msg_packet.c messenger/tests/msg_mesh_test.c -o /tmp/msg-mesh-test && /tmp/msg-mesh-test
```

(The sources `#include "messenger/msg_*.h"`, so compile with `-I.` from the
folder that contains `messenger/`.)

## Integrating into a new project

1. Add `messenger/*.c` to your build; add the folder's parent to the include path.
2. Implement the four `msg_radio.h` hooks for your radio:
   - `msg_radio_set_call(call)` — set the local callsign (GGM2 "from").
   - `msg_radio_rx_arm()` — arm/keep FSK receive; decoded frames → `msg_store`.
   - `msg_radio_poll()` — return true once per newly decoded frame (UI refresh).
   - `msg_radio_send(text)` — key TX, send one GGM2 frame, store a Sent copy.
   On a BK4819 target, implement these over `drivers/bk4819_fsk.*`:
   `bk4819_fsk_setup()` once, `bk4819_fsk_tx()` to send, and
   `bk4819_fsk_rx_service()` on every radio tick to drain the 8-word FIFO
   (it overflows long before a 100-byte frame ends — do not read only at the end).
3. Call `msg_store_*` to render conversations; the UI never touches the radio
   directly, only the four hooks above.

## On-air format (for interop)

- Packet: 94 bytes, GGM2 layout (see `msg_packet.h`), its own CRC inside.
- Frame: 50 BK4819 FIFO words = `0xABCD` + 47 packet words (little-endian) +
  CRC-CCITT over the 94 bytes + `0xDCBA`. Must match GOGUFW exactly.

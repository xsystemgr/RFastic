<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Messenger link: a GGM2 modem on a UART

Version 1. Status: implemented and host-tested on both ends, not yet tried on hardware.

A radio with a BK4819 FSK modem already sends and receives GGM2 frames (the GOGUFW-compatible analog messenger,
[msg_packet.h](../messenger/msg_packet.h)). The messenger link offers that modem to a host on the radio's UART, the
one the programming cable uses. The host is usually a small ESP32 wired inside the radio, but a PC on a cable works
too. With the link, the radio only moves GGM2 packets on and off the air. The host owns everything else: relaying,
dedup, ACKs, retries, the message log, and the phone UI.

The over-the-air format does not change. A radio with a host still talks to a plain GOGUFW radio, and the plain radio
sees ordinary GGM2 TEXT and ACK frames.

```
phone ──WiFi/BLE──► ESP32 (mesh host) ──UART 38400 8N1──► radio MCU ──SPI──► BK4819 ──► 1200-baud FSK, GGM2
```

## 1. Framing

The link uses KISS framing, the public TNC host protocol. `FEND 0xC0` starts and ends every frame. Inside a frame,
`0xC0` is sent as `0xDB 0xDC` and `0xDB` as `0xDB 0xDD`. The first byte of a frame is the KISS command byte: the high
nibble is the port and the low nibble is the command.

The link uses **KISS port 1**. Port 0 stays free for an APRS TNC on radios that have one, so both can share the UART.
A receiver ignores frames for other ports.

| Command byte | Meaning |
|---|---|
| `0x10` | data (port 1, command 0): one GGM2 packet, host to radio |
| `0x16` | SetHardware (port 1, command 6): control, followed by an op byte |

## 2. Host to radio

| Frame | Meaning |
|---|---|
| `0x10` + 94 bytes | Transmit this GGM2 packet on the current channel. The radio checks length, magic, version and CRC first. |
| `0x16 0x01` + version | HELLO. The radio enters bridge mode with the default mode and answers with a HELLO reply. |
| `0x16 0x02` + 1..8 bytes | SET_CALL: the radio's own callsign, for its UI and for any frame it builds itself. |
| `0x16 0x03` | KEEPALIVE. Gets no reply. Every frame from the host counts as a keepalive. |
| `0x16 0x04` + mode | SET_MODE. Answered with a HELLO reply. |

Mode bits:

| Bit | Name | When set |
|---|---|---|
| 0 | BRIDGE | The radio sends no ACK of its own for received TEXT. The host does it. |
| 1 | STORE | The radio still files received TEXT in its own inbox, so its screen shows them. |

HELLO sets BRIDGE and STORE.

## 3. Radio to host

| Frame | Meaning |
|---|---|
| `0x16 0x80` + rssi (int8, dBm) + 94 bytes | A valid GGM2 frame was received: the packet as it came off the air. |
| `0x16 0x81` + version + mode + caps + call (8 bytes, zero padded) | HELLO reply. |
| `0x16 0x82` + type + id (LE16) + result | Transmit result for a data frame. |
| `0x16 0x8F` + op + error | Request refused. op is `0x10` for a data frame, otherwise the control op. |

The radio reports every valid frame, whatever its type or destination. Filtering is the host's job.

Capability bits: `0x01` TX (the radio can transmit here) and `0x02` LBT (it waits for a clear channel before keying
up).

Transmit results: `0` OK, `1` BUSY (the channel stayed busy), `2` REFUSED (no TX here: out of a TX band, receive-only,
TX inhibited), `3` QUEUE_FULL (a transmit is already pending: send again after its result), `4` TIMEOUT (the burst
never finished).

Errors: `1` BAD_PACKET, `2` BAD_OP, `3` BAD_LEN.

## 4. Session

1. The host sends HELLO until it gets a HELLO reply (every 3 s), then sends SET_CALL if the reply's callsign is not
   its own.
2. The host keeps one transmit in flight. It sends the next data frame only after the previous one's result, or 5 s
   without a result. On QUEUE_FULL it retries after about 300 ms. On BUSY it retries a few times after a random 0.5 to
   2 s.
3. The host sends something at least every 5 s. It repeats HELLO every 30 s and treats a missing reply (3 s) as the
   radio being gone.
4. The radio leaves bridge mode after **15 s** with no frame from the host. It then ACKs and files messages on its own
   again, so a host that crashes or loses power never leaves the radio deaf.
5. While no host is attached, the radio queues nothing for it.

The radio sends its output in whole frames only, so frames from different tasks never interleave on the wire. If its
output queue is full, it drops a frame whole and counts it rather than truncating it.

## 5. Mesh (host side)

The host runs the mesh ([msg_mesh.h](../messenger/msg_mesh.h)) on the plain GGM2 fields. It adds no new bytes.

- A frame's identity is `(from, id, type)`, remembered for 120 s. A frame already seen is a duplicate.
- `hops = ttl_init - ttl_remain`. A first-hand frame has `hops = 0`.
- **TEXT to us:** delivered once and ACKed with `ttl = hops + 1`, so the ACK can travel back over the same number of
  relays. A duplicate TEXT to us is ACKed again, because the sender is retrying and our first ACK may have been lost,
  but it is not delivered twice.
- **TEXT to ALL:** delivered once. It is ACKed with `ttl = 1` only when heard first-hand, which is what a plain GOGUFW
  radio does. That ACK marks a broadcast as "heard by someone".
- **Relay:** TEXT not addressed to us, and ACK not addressed to us, are forwarded while `ttl_remain > 1`. The copy
  carries `ttl_remain - 1` (the CRC is recomputed) and goes out after a random delay of 200 ms plus a random slot of
  1.2 s (4 slots) plus up to 99 ms of jitter. A slot is longer than one burst (about 0.9 s on air), so the earliest
  relay is usually heard before the others fire. A node that hears another copy of a frame it is about to relay
  cancels its own copy.
- PING, PONG and WAKE are neighbor-only and never relayed.
- **Own TEXT:** sent with `ttl = 3` by default, which is GOGUFW's default. A direct TEXT is retried twice (three tries
  in all) with the same id until an ACK comes back. The ACK window is 3 s + 4.5 s per hop of budget, plus a random
  0.2 to 0.8 s before each retry. A broadcast is not retried: after one window it is "no ack" unless some neighbor
  ACKed it.
- A direct TEXT carries the destination callsign in `to`. A plain GOGUFW radio shows any TEXT it receives, so direct
  messages are private by convention only. Nothing on the air is encrypted.

### What a plain GOGUFW radio sees

- TEXT from a host node: an ordinary message (`to` = ALL or a callsign).
- Relayed TEXT: the same message, once, with the original sender's callsign, because its `(from, id)` is unchanged.
  Whether it suppresses the duplicate depends on its own dedup.
- ACK with `ttl > 1`: an ordinary ACK.

## 6. Timing at 1200 baud

One GGM2 frame is 100 FIFO bytes plus a 16-byte preamble and sync, about 0.8 s, plus about 80 ms of keying and
settling: about 0.9 s per burst. A direct message over one relay with its ACK back is four bursts plus two relay
delays, typically 5 to 8 s.

## 7. Not in version 1

- Listen-before-talk on the radio side (the capability bit exists).
- BLE transport on the host (WiFi access point and USB serial only for now).
- Fragmentation of texts longer than 64 bytes.
- Any encryption. On amateur bands, encryption that obscures meaning is not allowed, so if it ever appears it must be
  opt-in and off by default.

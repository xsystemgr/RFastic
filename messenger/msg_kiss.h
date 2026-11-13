// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// KISS framing (the public TNC host protocol): FEND 0xC0 delimits frames, FESC 0xDB escapes FEND/FESC inside them.
// The first byte of a frame is the command: high nibble = port, low nibble = command (0 = data, 6 = SetHardware).
// Used as the byte framing of the messenger link (msg_link.h) between a radio and its host (an ESP32, a PC).
#ifndef MSG_KISS_H
#define MSG_KISS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MSG_KISS_FEND		0xC0u
#define MSG_KISS_FESC		0xDBu
#define MSG_KISS_TFEND		0xDCu
#define MSG_KISS_TFESC		0xDDu
#define MSG_KISS_MAX_FRAME	128u	// command byte + data, unescaped; the link's largest frame is 98 bytes

typedef struct {
	uint8_t  buf[MSG_KISS_MAX_FRAME];	// command byte + data of the frame in progress / just completed
	uint16_t len;
	bool     in_frame;			// a FEND opened a frame and bytes are being collected
	bool     esc;			// the previous byte was FESC
	bool     overflow;			// the frame outgrew buf: it is dropped at its closing FEND
} msg_kiss_rx_t;

void msg_kiss_rx_init(msg_kiss_rx_t *rx);

// Feed one received byte. Returns the length of a frame that just completed (buf[0] = command, then the data), or 0.
// Empty frames (FEND FEND) and overflowed frames are dropped. The returned frame stays valid until the next byte.
uint16_t msg_kiss_rx_byte(msg_kiss_rx_t *rx, uint8_t b);

// Encode one frame: FEND, cmd, head[0..hlen), data[0..dlen) (both escaped), FEND. head or data may be NULL with length
// 0. Returns the bytes written, or 0 when out_max is too small (nothing useful is written then).
size_t msg_kiss_encode(uint8_t *out, size_t out_max, uint8_t cmd, const uint8_t *head, size_t hlen,
	const uint8_t *data, size_t dlen);

// Worst-case encoded size for a frame of n unescaped bytes after the command byte.
#define MSG_KISS_ENCODED_MAX(n)	(2u * ((n) + 1u) + 2u)

#endif

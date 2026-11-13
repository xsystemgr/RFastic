// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// KISS framing (msg_kiss.h).
#include "messenger/msg_kiss.h"

void msg_kiss_rx_init(msg_kiss_rx_t *rx)
{
	rx->len = 0;
	rx->in_frame = false;
	rx->esc = false;
	rx->overflow = false;
}

static void put(msg_kiss_rx_t *rx, uint8_t b)
{
	if (rx->len < MSG_KISS_MAX_FRAME) {
		rx->buf[rx->len++] = b;
	} else {
		rx->overflow = true;
	}
}

uint16_t msg_kiss_rx_byte(msg_kiss_rx_t *rx, uint8_t b)
{
	if (b == MSG_KISS_FEND) {
		uint16_t done = (rx->in_frame && !rx->overflow) ? rx->len : 0;
		// A FEND both closes the frame in progress and opens the next one (back-to-back frames share it).
		rx->in_frame = true;
		rx->esc = false;
		rx->overflow = false;
		rx->len = 0;
		return done;
	}
	if (!rx->in_frame) {
		return 0;			// noise before the first FEND
	}
	if (rx->esc) {
		rx->esc = false;
		if (b == MSG_KISS_TFEND) {
			put(rx, MSG_KISS_FEND);
		} else if (b == MSG_KISS_TFESC) {
			put(rx, MSG_KISS_FESC);
		} else {
			rx->overflow = true;	// a protocol error: drop the frame at its closing FEND
		}
		return 0;
	}
	if (b == MSG_KISS_FESC) {
		rx->esc = true;
		return 0;
	}
	put(rx, b);
	return 0;
}

static bool emit(uint8_t *out, size_t out_max, size_t *n, uint8_t b)
{
	if (b == MSG_KISS_FEND || b == MSG_KISS_FESC) {
		if (*n + 2u > out_max) {
			return false;
		}
		out[(*n)++] = MSG_KISS_FESC;
		out[(*n)++] = (b == MSG_KISS_FEND) ? MSG_KISS_TFEND : MSG_KISS_TFESC;
		return true;
	}
	if (*n + 1u > out_max) {
		return false;
	}
	out[(*n)++] = b;
	return true;
}

size_t msg_kiss_encode(uint8_t *out, size_t out_max, uint8_t cmd, const uint8_t *head, size_t hlen,
	const uint8_t *data, size_t dlen)
{
	size_t n = 0;
	if (out == NULL || out_max < 3u) {
		return 0;
	}
	out[n++] = MSG_KISS_FEND;
	if (!emit(out, out_max, &n, cmd)) {
		return 0;
	}
	for (size_t i = 0; i < hlen; i++) {
		if (!emit(out, out_max, &n, head[i])) {
			return 0;
		}
	}
	for (size_t i = 0; i < dlen; i++) {
		if (!emit(out, out_max, &n, data[i])) {
			return 0;
		}
	}
	if (n + 1u > out_max) {
		return 0;
	}
	out[n++] = MSG_KISS_FEND;
	return n;
}

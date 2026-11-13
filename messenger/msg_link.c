// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The messenger link, host side (msg_link.h).
#include <string.h>
#include "messenger/msg_link.h"
#include "messenger/msg_kiss.h"

size_t msg_link_encode_tx(uint8_t *out, size_t out_max, const uint8_t packet[MSG_PKT_WIRE_LEN])
{
	return msg_kiss_encode(out, out_max, MSG_LINK_CMD_DATA, NULL, 0, packet, MSG_PKT_WIRE_LEN);
}

static size_t ctrl(uint8_t *out, size_t out_max, uint8_t op, const uint8_t *data, size_t len)
{
	return msg_kiss_encode(out, out_max, MSG_LINK_CMD_CTRL, &op, 1, data, len);
}

size_t msg_link_encode_hello(uint8_t *out, size_t out_max)
{
	uint8_t v = MSG_LINK_VERSION;
	return ctrl(out, out_max, MSG_LINK_OP_HELLO, &v, 1);
}

size_t msg_link_encode_set_call(uint8_t *out, size_t out_max, const char *call)
{
	size_t n = 0;
	while (call != NULL && call[n] != '\0' && n < MSG_CALLSIGN_LEN) {
		n++;
	}
	if (n == 0) {
		return 0;
	}
	return ctrl(out, out_max, MSG_LINK_OP_SET_CALL, (const uint8_t *)call, n);
}

size_t msg_link_encode_keepalive(uint8_t *out, size_t out_max)
{
	return ctrl(out, out_max, MSG_LINK_OP_KEEPALIVE, NULL, 0);
}

size_t msg_link_encode_set_mode(uint8_t *out, size_t out_max, uint8_t mode)
{
	return ctrl(out, out_max, MSG_LINK_OP_SET_MODE, &mode, 1);
}

bool msg_link_decode_event(const uint8_t *frame, uint16_t len, msg_link_event_t *ev)
{
	memset(ev, 0, sizeof *ev);
	if (frame == NULL || len < 2u || frame[0] != MSG_LINK_CMD_CTRL) {
		return false;
	}
	const uint8_t *d = &frame[2];
	uint16_t n = (uint16_t)(len - 2u);
	switch (frame[1]) {
	case MSG_LINK_EV_RX:
		if (n != 1u + MSG_PKT_WIRE_LEN) {
			return false;
		}
		ev->kind = MSG_LINK_EVENT_RX;
		ev->rssi = (int8_t)d[0];
		memcpy(ev->packet, &d[1], MSG_PKT_WIRE_LEN);
		return true;
	case MSG_LINK_EV_HELLO:
		if (n < 3u || n > 3u + MSG_CALLSIGN_LEN) {
			return false;
		}
		ev->kind = MSG_LINK_EVENT_HELLO;
		ev->version = d[0];
		ev->mode = d[1];
		ev->caps = d[2];
		for (uint16_t i = 3; i < n && d[i] != 0; i++) {
			ev->call[i - 3u] = (char)d[i];
		}
		return true;
	case MSG_LINK_EV_TX_RESULT:
		if (n != 4u) {
			return false;
		}
		ev->kind = MSG_LINK_EVENT_TX_RESULT;
		ev->tx_type = d[0];
		ev->tx_id = (uint16_t)(d[1] | ((uint16_t)d[2] << 8));
		ev->result = d[3];
		return true;
	case MSG_LINK_EV_ERROR:
		if (n != 2u) {
			return false;
		}
		ev->kind = MSG_LINK_EVENT_ERROR;
		ev->op = d[0];
		ev->error = d[1];
		return true;
	default:
		return false;
	}
}

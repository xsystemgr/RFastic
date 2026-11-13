// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's side of the messenger link (msg_bridge.h).
#include <string.h>
#include "messenger/msg_bridge.h"
#include "messenger/msg_link.h"
#include "messenger/msg_kiss.h"

static msg_bridge_ops_t ops;
static msg_kiss_rx_t rx;
static char own_call[MSG_CALLSIGN_LEN + 1];
static bool attached;
static uint8_t mode = MSG_LINK_MODE_DEFAULT;
static uint32_t last_host_ms;
static uint16_t dropped;

// Output queue: a byte ring of whole encoded frames plus an index of their lengths. A producer appends a frame only
// when all of it fits; output() hands out only whole frames that fit its buffer, so frames from the link task and the
// radio task never interleave on the wire.
#define OUT_FRAMES_MAX	16u
static uint8_t out_ring[MSG_BRIDGE_OUT_BYTES];
static uint16_t out_head, out_tail, out_used;		// bytes
static uint16_t frame_len[OUT_FRAMES_MAX];		// lengths of the queued frames, oldest first
static uint8_t frame_head, frame_count;

static void lock(void)
{
	if (ops.lock != NULL) {
		ops.lock(ops.ctx);
	}
}

static void unlock(void)
{
	if (ops.unlock != NULL) {
		ops.unlock(ops.ctx);
	}
}

// Append one encoded frame. Caller holds the lock. False (and counted) when it doesn't fit.
static bool out_push(const uint8_t *bytes, size_t n)
{
	if (n == 0 || n > MSG_BRIDGE_OUT_BYTES - out_used || frame_count >= OUT_FRAMES_MAX) {
		dropped++;
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		out_ring[out_tail] = bytes[i];
		out_tail = (uint16_t)((out_tail + 1u) % MSG_BRIDGE_OUT_BYTES);
	}
	out_used = (uint16_t)(out_used + n);
	frame_len[(frame_head + frame_count) % OUT_FRAMES_MAX] = (uint16_t)n;
	frame_count++;
	return true;
}

static void send_ctrl(uint8_t op, const uint8_t *data, size_t len)
{
	uint8_t enc[MSG_LINK_FRAME_MAX];
	size_t n = msg_kiss_encode(enc, sizeof enc, MSG_LINK_CMD_CTRL, &op, 1, data, len);
	lock();
	(void)out_push(enc, n);
	unlock();
}

static void send_error(uint8_t op, uint8_t err)
{
	uint8_t d[2] = { op, err };
	send_ctrl(MSG_LINK_EV_ERROR, d, sizeof d);
}

static void send_hello_reply(void)
{
	uint8_t d[3 + MSG_CALLSIGN_LEN];
	memset(d, 0, sizeof d);
	d[0] = MSG_LINK_VERSION;
	d[1] = mode;
	d[2] = (ops.caps != NULL) ? ops.caps(ops.ctx) : MSG_LINK_CAP_TX;
	memcpy(&d[3], own_call, strlen(own_call));
	send_ctrl(MSG_LINK_EV_HELLO, d, sizeof d);
}

void msg_bridge_init(const msg_bridge_ops_t *o, const char *call)
{
	memset(&ops, 0, sizeof ops);
	if (o != NULL) {
		ops = *o;
	}
	msg_kiss_rx_init(&rx);
	memset(own_call, 0, sizeof own_call);
	for (unsigned i = 0; call != NULL && call[i] != '\0' && i < MSG_CALLSIGN_LEN; i++) {
		own_call[i] = call[i];
	}
	attached = false;
	mode = MSG_LINK_MODE_DEFAULT;
	last_host_ms = 0;
	dropped = 0;
	out_head = out_tail = out_used = 0;
	frame_head = frame_count = 0;
}

bool msg_bridge_claims(uint8_t b)
{
	return b == MSG_KISS_FEND;
}

static void on_data(const uint8_t *d, uint16_t n)
{
	msg_packet_t p;
	if (n != MSG_PKT_WIRE_LEN || !msg_packet_parse(d, (uint8_t)n, &p)) {
		send_error(MSG_LINK_CMD_DATA, MSG_LINK_ERR_BAD_PACKET);
		return;
	}
	if (ops.tx_packet == NULL || !ops.tx_packet(ops.ctx, d)) {
		uint8_t r[4] = { p.type, (uint8_t)(p.id & 0xFFu), (uint8_t)(p.id >> 8), MSG_LINK_TX_QUEUE_FULL };
		send_ctrl(MSG_LINK_EV_TX_RESULT, r, sizeof r);
	}
}

static void on_ctrl(const uint8_t *d, uint16_t n)
{
	if (n < 1u) {
		return;
	}
	uint8_t op = d[0];
	const uint8_t *a = &d[1];
	uint16_t an = (uint16_t)(n - 1u);
	switch (op) {
	case MSG_LINK_OP_HELLO:
		mode = MSG_LINK_MODE_DEFAULT;
		attached = true;
		send_hello_reply();
		break;
	case MSG_LINK_OP_SET_CALL:
		if (an < 1u || an > MSG_CALLSIGN_LEN) {
			send_error(op, MSG_LINK_ERR_BAD_LEN);
			break;
		}
		memset(own_call, 0, sizeof own_call);
		memcpy(own_call, a, an);
		if (ops.set_call != NULL) {
			ops.set_call(ops.ctx, own_call);
		}
		break;
	case MSG_LINK_OP_KEEPALIVE:
		break;
	case MSG_LINK_OP_SET_MODE:
		if (an != 1u) {
			send_error(op, MSG_LINK_ERR_BAD_LEN);
			break;
		}
		mode = a[0];
		send_hello_reply();
		break;
	default:
		send_error(op, MSG_LINK_ERR_BAD_OP);
		break;
	}
}

static void on_frame(const uint8_t *f, uint16_t len, uint32_t now_ms)
{
	if ((f[0] >> 4) != MSG_LINK_PORT) {
		return;				// another KISS port: not ours
	}
	last_host_ms = now_ms;
	if (f[0] == MSG_LINK_CMD_DATA) {
		if (!attached) {
			attached = true;	// a host that transmits without HELLO still gets bridge semantics
		}
		on_data(&f[1], (uint16_t)(len - 1u));
	} else if (f[0] == MSG_LINK_CMD_CTRL) {
		on_ctrl(&f[1], (uint16_t)(len - 1u));
	}
}

void msg_bridge_feed(const uint8_t *buf, size_t len, uint32_t now_ms)
{
	for (size_t i = 0; i < len; i++) {
		uint16_t n = msg_kiss_rx_byte(&rx, buf[i]);
		if (n > 0) {
			on_frame(rx.buf, n, now_ms);
		}
	}
}

bool msg_bridge_in_frame(void)
{
	return rx.in_frame && rx.len > 0;
}

size_t msg_bridge_output(uint8_t *buf, size_t max)
{
	size_t n = 0;
	lock();
	while (frame_count > 0 && n + frame_len[frame_head] <= max) {
		uint16_t fl = frame_len[frame_head];
		for (uint16_t i = 0; i < fl; i++) {
			buf[n++] = out_ring[out_head];
			out_head = (uint16_t)((out_head + 1u) % MSG_BRIDGE_OUT_BYTES);
		}
		out_used = (uint16_t)(out_used - fl);
		frame_head = (uint8_t)((frame_head + 1u) % OUT_FRAMES_MAX);
		frame_count--;
	}
	unlock();
	return n;
}

void msg_bridge_tick(uint32_t now_ms)
{
	if (attached && (uint32_t)(now_ms - last_host_ms) > MSG_LINK_TIMEOUT_MS) {
		attached = false;		// the host went away: the radio is a standalone messenger again
		mode = MSG_LINK_MODE_DEFAULT;
		lock();
		out_head = out_tail = out_used = 0;
		frame_head = frame_count = 0;
		unlock();
	}
}

bool msg_bridge_attached(void)
{
	return attached;
}

bool msg_bridge_suppress_auto_ack(void)
{
	return attached && (mode & MSG_LINK_MODE_BRIDGE) != 0;
}

bool msg_bridge_local_store(void)
{
	return !attached || (mode & MSG_LINK_MODE_STORE) != 0;
}

void msg_bridge_on_rx(const uint8_t packet[MSG_PKT_WIRE_LEN], int8_t rssi)
{
	if (!attached) {
		return;
	}
	uint8_t head[2] = { MSG_LINK_EV_RX, (uint8_t)rssi };
	uint8_t enc[MSG_LINK_FRAME_MAX];
	size_t n = msg_kiss_encode(enc, sizeof enc, MSG_LINK_CMD_CTRL, head, sizeof head, packet, MSG_PKT_WIRE_LEN);
	lock();
	(void)out_push(enc, n);
	unlock();
}

void msg_bridge_on_tx_done(const uint8_t packet[MSG_PKT_WIRE_LEN], uint8_t result)
{
	if (!attached) {
		return;
	}
	uint8_t r[4] = { packet[5], packet[7], packet[8], result };	// type, id LE (wire offsets, msg_packet.h)
	send_ctrl(MSG_LINK_EV_TX_RESULT, r, sizeof r);
}

uint16_t msg_bridge_dropped(void)
{
	return dropped;
}

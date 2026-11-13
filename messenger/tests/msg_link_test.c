// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host test: KISS framing, the host side of the messenger link and the radio-side bridge talking to each other.
#include <stdio.h>
#include <string.h>
#include "messenger/msg_kiss.h"
#include "messenger/msg_link.h"
#include "messenger/msg_bridge.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ---- a fake board --------------------------------------------------------------------------------------------------
static uint8_t board_tx[MSG_PKT_WIRE_LEN];
static int board_tx_count;
static bool board_tx_busy;
static char board_call[MSG_CALLSIGN_LEN + 1];

static bool fake_tx(void *ctx, const uint8_t p[MSG_PKT_WIRE_LEN])
{
	(void)ctx;
	if (board_tx_busy) {
		return false;
	}
	memcpy(board_tx, p, MSG_PKT_WIRE_LEN);
	board_tx_count++;
	board_tx_busy = true;
	return true;
}

static void fake_call(void *ctx, const char *c)
{
	(void)ctx;
	strncpy(board_call, c, MSG_CALLSIGN_LEN);
}

static uint8_t fake_caps(void *ctx)
{
	(void)ctx;
	return MSG_LINK_CAP_TX | MSG_LINK_CAP_LBT;
}

// Pull every frame the bridge has for the host and decode the events.
static int host_read(msg_link_event_t *evs, int max)
{
	static msg_kiss_rx_t krx;
	static bool init;
	if (!init) {
		msg_kiss_rx_init(&krx);
		init = true;
	}
	uint8_t buf[256];
	int n = 0;
	size_t got;
	while ((got = msg_bridge_output(buf, sizeof buf)) > 0) {
		for (size_t i = 0; i < got; i++) {
			uint16_t fl = msg_kiss_rx_byte(&krx, buf[i]);
			if (fl > 0 && n < max && msg_link_decode_event(krx.buf, fl, &evs[n])) {
				n++;
			}
		}
	}
	return n;
}

static void test_kiss(void)
{
	// Every byte value, including FEND/FESC, survives a round trip.
	uint8_t data[256];
	for (int i = 0; i < 100; i++) {
		data[i] = (uint8_t)(i * 37u);
	}
	data[3] = MSG_KISS_FEND;
	data[4] = MSG_KISS_FESC;
	uint8_t enc[MSG_KISS_ENCODED_MAX(100)];
	size_t n = msg_kiss_encode(enc, sizeof enc, 0x10, NULL, 0, data, 100);
	CHECK(n > 102);
	CHECK(enc[0] == MSG_KISS_FEND && enc[n - 1] == MSG_KISS_FEND);
	for (size_t i = 1; i + 1 < n; i++) {
		CHECK(enc[i] != MSG_KISS_FEND);
	}
	msg_kiss_rx_t rx;
	msg_kiss_rx_init(&rx);
	CHECK(msg_kiss_rx_byte(&rx, 0x55) == 0);	// noise before the first FEND is ignored
	uint16_t got = 0;
	for (size_t i = 0; i < n; i++) {
		uint16_t r = msg_kiss_rx_byte(&rx, enc[i]);
		if (r) {
			got = r;
		}
	}
	CHECK(got == 101);
	CHECK(rx.buf[0] == 0x10 && memcmp(&rx.buf[1], data, 100) == 0);
	// Too small an output buffer writes nothing useful.
	CHECK(msg_kiss_encode(enc, 10, 0x10, NULL, 0, data, 100) == 0);
	// A bad escape drops the frame.
	msg_kiss_rx_init(&rx);
	uint8_t bad[] = { MSG_KISS_FEND, 0x10, MSG_KISS_FESC, 0x01, 0x02, MSG_KISS_FEND };
	got = 0;
	for (size_t i = 0; i < sizeof bad; i++) {
		got |= msg_kiss_rx_byte(&rx, bad[i]);
	}
	CHECK(got == 0);
}

static void test_bridge(void)
{
	msg_bridge_ops_t ops = { .tx_packet = fake_tx, .set_call = fake_call, .caps = fake_caps };
	msg_bridge_init(&ops, "RT880");
	msg_link_event_t ev[8];
	uint8_t f[MSG_LINK_FRAME_MAX];
	uint8_t pkt[MSG_PKT_WIRE_LEN];
	msg_packet_build_text(pkt, sizeof pkt, 0x0102, "SV4QXF", "hi", 3);

	// Detached: nothing is queued for a host that isn't there; the radio ACKs and stores by itself.
	CHECK(!msg_bridge_attached());
	CHECK(!msg_bridge_suppress_auto_ack() && msg_bridge_local_store());
	msg_bridge_on_rx(pkt, -80);
	CHECK(host_read(ev, 8) == 0);

	// HELLO attaches and is answered with version, mode, caps, call.
	size_t n = msg_link_encode_hello(f, sizeof f);
	msg_bridge_feed(f, n, 1000);
	CHECK(msg_bridge_attached() && msg_bridge_suppress_auto_ack() && msg_bridge_local_store());
	CHECK(host_read(ev, 8) == 1);
	CHECK(ev[0].kind == MSG_LINK_EVENT_HELLO && ev[0].version == MSG_LINK_VERSION);
	CHECK(ev[0].mode == MSG_LINK_MODE_DEFAULT && ev[0].caps == (MSG_LINK_CAP_TX | MSG_LINK_CAP_LBT));
	CHECK(strcmp(ev[0].call, "RT880") == 0);

	// SET_CALL reaches the board; SET_MODE without STORE stops local filing.
	n = msg_link_encode_set_call(f, sizeof f, "SV4QXF");
	msg_bridge_feed(f, n, 1100);
	CHECK(strcmp(board_call, "SV4QXF") == 0);
	n = msg_link_encode_set_mode(f, sizeof f, MSG_LINK_MODE_BRIDGE);
	msg_bridge_feed(f, n, 1200);
	CHECK(!msg_bridge_local_store());
	CHECK(host_read(ev, 8) == 1 && ev[0].kind == MSG_LINK_EVENT_HELLO && strcmp(ev[0].call, "SV4QXF") == 0);

	// Received frames go to the host with their RSSI, byte for byte.
	msg_bridge_on_rx(pkt, -97);
	CHECK(host_read(ev, 8) == 1);
	CHECK(ev[0].kind == MSG_LINK_EVENT_RX && ev[0].rssi == -97 && memcmp(ev[0].packet, pkt, sizeof pkt) == 0);

	// A transmit request is validated and handed to the board; a second one while busy is QUEUE_FULL.
	n = msg_link_encode_tx(f, sizeof f, pkt);
	msg_bridge_feed(f, n, 1300);
	CHECK(board_tx_count == 1 && memcmp(board_tx, pkt, sizeof pkt) == 0);
	msg_bridge_feed(f, n, 1310);
	CHECK(board_tx_count == 1);
	CHECK(host_read(ev, 8) == 1 && ev[0].kind == MSG_LINK_EVENT_TX_RESULT && ev[0].result == MSG_LINK_TX_QUEUE_FULL);
	board_tx_busy = false;
	msg_bridge_on_tx_done(pkt, MSG_LINK_TX_OK);
	CHECK(host_read(ev, 8) == 1);
	CHECK(ev[0].kind == MSG_LINK_EVENT_TX_RESULT && ev[0].tx_id == 0x0102 && ev[0].tx_type == MSG_PKT_TYPE_TEXT);
	CHECK(ev[0].result == MSG_LINK_TX_OK);

	// A corrupted packet is refused with BAD_PACKET and never reaches the board.
	uint8_t bad[MSG_PKT_WIRE_LEN];
	memcpy(bad, pkt, sizeof bad);
	bad[30] ^= 0x40;
	n = msg_link_encode_tx(f, sizeof f, bad);
	msg_bridge_feed(f, n, 1400);
	CHECK(board_tx_count == 1);
	CHECK(host_read(ev, 8) == 1 && ev[0].kind == MSG_LINK_EVENT_ERROR && ev[0].error == MSG_LINK_ERR_BAD_PACKET);

	// Byte-at-a-time feeding (a UART ISR) works the same.
	n = msg_link_encode_keepalive(f, sizeof f);
	for (size_t i = 0; i < n; i++) {
		msg_bridge_feed(&f[i], 1, 5000);
	}
	CHECK(host_read(ev, 8) == 0);

	// The output never splits a frame: a too-small read gets nothing, a big one gets whole frames.
	msg_bridge_on_rx(pkt, -50);
	uint8_t small[20];
	CHECK(msg_bridge_output(small, sizeof small) == 0);
	CHECK(host_read(ev, 8) == 1 && ev[0].kind == MSG_LINK_EVENT_RX);

	// A full queue drops (and counts) instead of corrupting.
	for (int i = 0; i < 20; i++) {
		msg_bridge_on_rx(pkt, -60);
	}
	CHECK(msg_bridge_dropped() > 0);
	int got = host_read(ev, 8);
	CHECK(got >= 3);
	for (int i = 0; i < got; i++) {
		CHECK(ev[i].kind == MSG_LINK_EVENT_RX && memcmp(ev[i].packet, pkt, sizeof pkt) == 0);
	}

	// Silence for longer than the timeout detaches: the radio goes back to standalone behavior.
	msg_bridge_tick(5000 + MSG_LINK_TIMEOUT_MS - 1);
	CHECK(msg_bridge_attached());
	msg_bridge_tick(5000 + MSG_LINK_TIMEOUT_MS + 1);
	CHECK(!msg_bridge_attached() && !msg_bridge_suppress_auto_ack() && msg_bridge_local_store());

	// Frames for another KISS port (an APRS TNC on port 0) are not the bridge's.
	uint8_t other[MSG_LINK_FRAME_MAX];
	n = msg_kiss_encode(other, sizeof other, 0x00, NULL, 0, pkt, sizeof pkt);
	msg_bridge_feed(other, n, 30000);
	CHECK(!msg_bridge_attached() && board_tx_count == 1);
}

int main(void)
{
	test_kiss();
	test_bridge();
	if (failures) {
		printf("msg_link test: %d failure(s)\n", failures);
		return 1;
	}
	printf("msg_link test: OK (KISS round-trip/escape/errors, HELLO/SET_CALL/SET_MODE, RX events, TX + QUEUE_FULL, "
		"BAD_PACKET, whole-frame output, overflow, timeout, foreign port)\n");
	return 0;
}

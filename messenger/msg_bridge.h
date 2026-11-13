// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's side of the messenger link (msg_link.h, docs/bridge-protocol.md): it parses the host's KISS frames,
// hands packets to transmit to the board, and queues received frames and transmit results for the host.
//
// No board or RTOS header: the board passes its hooks in msg_bridge_ops_t. The byte side (feed/output/in_frame) has
// the shape of the host link's TNC slot (core/hostlink: claims/feed/in_frame/output), so a board with a host link
// registers it there; a board without one polls its UART and calls feed/output itself.
//
// Tasks: feed/output/tick run on the link side (one task); on_rx/on_tx_done may run on the radio task. The output
// queue is the only shared state; it is guarded by ops.lock/unlock when the two sides are different tasks.
#ifndef MSG_BRIDGE_H
#define MSG_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "messenger/msg_packet.h"

#define MSG_BRIDGE_OUT_BYTES	640u	// output queue: about three RX events plus replies

typedef struct {
	// Queue one validated GGM2 packet for transmit on the radio's current channel. Called from feed(). Return false
	// when a transmit is already pending (the host gets QUEUE_FULL). The board reports the outcome later through
	// msg_bridge_on_tx_done().
	bool (*tx_packet)(void *ctx, const uint8_t packet[MSG_PKT_WIRE_LEN]);
	// The host set the radio's callsign (NUL-terminated, 1..8 chars). May be NULL.
	void (*set_call)(void *ctx, const char *call);
	// Capability bits for the HELLO reply (MSG_LINK_CAP_*). May be NULL (then TX only).
	uint8_t (*caps)(void *ctx);
	// Guard the output queue (NULL when feed/output and on_rx share one task).
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);
	void *ctx;
} msg_bridge_ops_t;

// Start (or restart) with the board's hooks and its current callsign. The bridge starts detached.
void msg_bridge_init(const msg_bridge_ops_t *ops, const char *call);

// Byte side (link task)
bool msg_bridge_claims(uint8_t b);				// true for FEND: a link frame starts
void msg_bridge_feed(const uint8_t *buf, size_t len, uint32_t now_ms);
bool msg_bridge_in_frame(void);
size_t msg_bridge_output(uint8_t *buf, size_t max);	// whole frames only; 0 when there is nothing to send
void msg_bridge_tick(uint32_t now_ms);			// leaves bridge mode after MSG_LINK_TIMEOUT_MS of silence

// State, for the board's messenger code (radio task)
bool msg_bridge_attached(void);		// a host said HELLO and is still talking
bool msg_bridge_suppress_auto_ack(void);	// attached and in BRIDGE mode: don't send the radio's own ACKs
bool msg_bridge_local_store(void);	// detached, or STORE mode: file received TEXT in the radio's inbox

// Events from the radio (radio task). Dropped while detached, or when the output queue is full (counted).
void msg_bridge_on_rx(const uint8_t packet[MSG_PKT_WIRE_LEN], int8_t rssi);
void msg_bridge_on_tx_done(const uint8_t packet[MSG_PKT_WIRE_LEN], uint8_t result);
uint16_t msg_bridge_dropped(void);

#endif

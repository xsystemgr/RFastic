// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A small store-and-forward mesh on top of GGM2, run by the link host (the ESP32): the radio is only its modem.
// Rules (docs/bridge-protocol.md, section "Mesh"):
//   - Dedup on (from, id, type), remembered MSG_MESH_SEEN_MS.
//   - TEXT and ACK are relayed while ttl_remain > 1, with ttl_remain - 1 and a random delay; a relay is cancelled if
//     another node's copy of the same frame is heard first (one copy per neighborhood is enough). The delay is a
//     random slot longer than a burst, so the earliest relay is heard before the others fire.
//   - TEXT to us: delivered and ACKed with a hop budget of the hops it took + 1, so the ACK comes back over relays.
//     A duplicate TEXT to us is ACKed again (our first ACK may have been lost) but not delivered again.
//   - TEXT to ALL: delivered; ACKed (budget 1) only when heard first-hand, as a plain GOGUFW radio does.
//   - Our own TEXT: retried until ACKed (direct) or one ACK window passes (to ALL).
//   - PING/PONG/WAKE are neighbor-only: never relayed.
// No globals: one msg_mesh_t per node, so host tests can run a whole network in one process.
#ifndef MSG_MESH_H
#define MSG_MESH_H

#include <stdint.h>
#include <stdbool.h>
#include "messenger/msg_packet.h"

#define MSG_MESH_SEEN_MAX	48u
#define MSG_MESH_SEEN_MS	120000u	// how long a frame key is remembered (a 16-bit id wraps much later)
#define MSG_MESH_RELAY_MAX	6u
#define MSG_MESH_PENDING_MAX	4u	// own messages waiting for an ACK

typedef enum {
	MSG_MESH_ST_SENT = 0,	// on the air (each try)
	MSG_MESH_ST_ACKED,	// delivered: an ACK came back
	MSG_MESH_ST_NO_ACK,	// to ALL: the window passed with no ACK (it may still have been heard)
	MSG_MESH_ST_FAILED,	// direct: every try went unanswered
} msg_mesh_status_t;

typedef struct {
	// Put one packet on the air (through the link). The mesh never sends two at once on its own; the host's link
	// queue serializes them.
	void (*send)(void *ctx, const uint8_t packet[MSG_PKT_WIRE_LEN]);
	// A TEXT for us or for ALL, first copy only.
	void (*deliver)(void *ctx, const msg_packet_t *p, int8_t rssi, uint8_t hops);
	// Progress of one of our own messages. May be NULL.
	void (*status)(void *ctx, uint16_t id, msg_mesh_status_t st);
	// Any valid frame heard first-hand or relayed (HEARD list, logging). May be NULL.
	void (*heard)(void *ctx, const msg_packet_t *p, int8_t rssi);
	uint32_t (*rand)(void *ctx);
	void *ctx;
} msg_mesh_ops_t;

typedef struct {
	char     call[MSG_CALLSIGN_LEN + 1];
	uint8_t  ttl;			// hop budget of our own TEXT (1 = neighbors only); GOGUFW uses 3
	bool     relay;			// forward other stations' TEXT/ACK
	bool     ack_broadcast;		// ACK first-hand TEXT to ALL (GOGUFW behavior)
	uint8_t  retries;		// extra tries for a direct TEXT
	// Relay delay = relay_min_ms + a random slot of relay_slot_ms (0..relay_slots-1) + 0..99 ms jitter. A slot is
	// longer than one burst, so a relay in an earlier slot is heard, and suppresses the others, before they fire.
	uint16_t relay_min_ms, relay_slot_ms;
	uint8_t  relay_slots;
	uint16_t ack_base_ms;		// ACK window = ack_base_ms + ack_hop_ms * ttl
	uint16_t ack_hop_ms;
} msg_mesh_cfg_t;

void msg_mesh_default_cfg(msg_mesh_cfg_t *cfg, const char *call);

typedef struct {
	uint8_t  key_from[MSG_CALLSIGN_LEN];
	uint16_t id;
	uint8_t  type;
	uint32_t at;
	bool     used;
} msg_mesh_seen_t;

typedef struct {
	bool     used;
	uint8_t  packet[MSG_PKT_WIRE_LEN];
	uint32_t due;
} msg_mesh_relay_t;

typedef struct {
	bool     used;
	bool     to_all;
	uint8_t  packet[MSG_PKT_WIRE_LEN];
	uint16_t id;
	uint8_t  tries;
	uint32_t deadline;
} msg_mesh_pending_t;

typedef struct {
	msg_mesh_cfg_t cfg;
	msg_mesh_ops_t ops;
	uint16_t next_id;
	msg_mesh_seen_t seen[MSG_MESH_SEEN_MAX];
	uint8_t  seen_next;
	msg_mesh_relay_t relay[MSG_MESH_RELAY_MAX];
	msg_mesh_pending_t pending[MSG_MESH_PENDING_MAX];
	uint16_t relayed, suppressed, duplicates;	// counters for a status page
} msg_mesh_t;

void msg_mesh_init(msg_mesh_t *m, const msg_mesh_cfg_t *cfg, const msg_mesh_ops_t *ops);
void msg_mesh_set_call(msg_mesh_t *m, const char *call);

// Send a TEXT (to NULL/""/"ALL" = broadcast). Returns false when the pending table is full or text is empty.
bool msg_mesh_send_text(msg_mesh_t *m, const char *to, const char *text, uint32_t now_ms, uint16_t *id_out);

// A frame came in from the radio.
void msg_mesh_on_rx(msg_mesh_t *m, const uint8_t packet[MSG_PKT_WIRE_LEN], int8_t rssi, uint32_t now_ms);

// Run timers (relays due, ACK windows). Call every 10..100 ms.
void msg_mesh_tick(msg_mesh_t *m, uint32_t now_ms);

#endif

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The messenger link: a radio's GGM2 modem offered to a host (an ESP32 inside the radio or on the programming cable,
// or a PC) over a UART, in KISS frames on KISS port 1. The radio only moves GGM2 packets on and off the air; the host
// owns the messaging protocol (relay, dedup, ACKs, retries, storage). Spec: docs/bridge-protocol.md.
//
//   host -> radio                                   radio -> host
//   0x10 <94-byte GGM2 packet>      transmit it     0x16 0x80 <rssi i8> <94-byte packet>   a frame was received
//   0x16 0x01 <version>             HELLO           0x16 0x81 <version> <mode> <caps> <call 8>   HELLO reply
//   0x16 0x02 <call, 1..8 bytes>    set callsign    0x16 0x82 <type> <id LE16> <result>   transmit result
//   0x16 0x03                       keepalive       0x16 0x8F <op> <error>                 request refused
//   0x16 0x04 <mode>                set mode
//
// This file holds the constants both ends share and the host's side (encode requests, decode the radio's events).
// The radio's side is msg_bridge.h.
#ifndef MSG_LINK_H
#define MSG_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "messenger/msg_packet.h"

#define MSG_LINK_VERSION	1u
#define MSG_LINK_PORT		1u			// KISS port; port 0 stays free for an APRS TNC
#define MSG_LINK_CMD_DATA	((MSG_LINK_PORT << 4) | 0x0u)	// 0x10
#define MSG_LINK_CMD_CTRL	((MSG_LINK_PORT << 4) | 0x6u)	// 0x16 (KISS SetHardware)

// host -> radio control ops
#define MSG_LINK_OP_HELLO	0x01u	// [version]: attach; the radio answers with HELLO_REPLY and enters bridge mode
#define MSG_LINK_OP_SET_CALL	0x02u	// [call 1..8]: the radio's own callsign (its UI and any frame it builds itself)
#define MSG_LINK_OP_KEEPALIVE	0x03u	// []: no reply; any frame from the host counts as a keepalive
#define MSG_LINK_OP_SET_MODE	0x04u	// [mode]: MSG_LINK_MODE_* bits; answered with HELLO_REPLY

// radio -> host events
#define MSG_LINK_EV_RX		0x80u
#define MSG_LINK_EV_HELLO	0x81u
#define MSG_LINK_EV_TX_RESULT	0x82u
#define MSG_LINK_EV_ERROR	0x8Fu

// Mode bits (HELLO sets MSG_LINK_MODE_DEFAULT)
#define MSG_LINK_MODE_BRIDGE	0x01u	// the radio does not answer TEXT with its own ACK: the host does
#define MSG_LINK_MODE_STORE	0x02u	// the radio still files received TEXT in its own inbox (its screen shows them)
#define MSG_LINK_MODE_DEFAULT	(MSG_LINK_MODE_BRIDGE | MSG_LINK_MODE_STORE)

// Capability bits in the HELLO reply
#define MSG_LINK_CAP_TX		0x01u	// the radio can transmit here (false on a receive-only build)
#define MSG_LINK_CAP_LBT	0x02u	// the radio waits for a clear channel before it keys up

// Transmit results
#define MSG_LINK_TX_OK		0u
#define MSG_LINK_TX_BUSY	1u	// the channel stayed busy (listen-before-talk gave up)
#define MSG_LINK_TX_REFUSED	2u	// no TX here (out of a TX band, receive-only, TX inhibited)
#define MSG_LINK_TX_QUEUE_FULL	3u	// a transmit is already pending: send again after its result
#define MSG_LINK_TX_TIMEOUT	4u	// the burst never finished

// Errors
#define MSG_LINK_ERR_BAD_PACKET	1u	// the data frame is not a valid GGM2 packet (length, magic, version, CRC)
#define MSG_LINK_ERR_BAD_OP	2u
#define MSG_LINK_ERR_BAD_LEN	3u

#define MSG_LINK_TIMEOUT_MS	15000u	// the radio leaves bridge mode after this long without a frame from the host
#define MSG_LINK_KEEPALIVE_MS	5000u	// how often the host should send something

// Largest encoded link frame (RX event: cmd, op, rssi, 94 bytes, all escaped, plus two FENDs)
#define MSG_LINK_FRAME_MAX	(2u * (3u + MSG_PKT_WIRE_LEN) + 2u)

// ---- host side ----------------------------------------------------------------------------------------------------
// Each returns the encoded byte count written to out (out_max >= MSG_LINK_FRAME_MAX is always enough), 0 on error.
size_t msg_link_encode_tx(uint8_t *out, size_t out_max, const uint8_t packet[MSG_PKT_WIRE_LEN]);
size_t msg_link_encode_hello(uint8_t *out, size_t out_max);
size_t msg_link_encode_set_call(uint8_t *out, size_t out_max, const char *call);
size_t msg_link_encode_keepalive(uint8_t *out, size_t out_max);
size_t msg_link_encode_set_mode(uint8_t *out, size_t out_max, uint8_t mode);

typedef enum {
	MSG_LINK_EVENT_NONE = 0,
	MSG_LINK_EVENT_RX,		// rssi, packet
	MSG_LINK_EVENT_HELLO,		// version, mode, caps, call
	MSG_LINK_EVENT_TX_RESULT,	// tx_type, tx_id, result
	MSG_LINK_EVENT_ERROR,		// op, error
} msg_link_event_kind_t;

typedef struct {
	msg_link_event_kind_t kind;
	int8_t   rssi;
	uint8_t  packet[MSG_PKT_WIRE_LEN];
	uint8_t  version, mode, caps;
	char     call[MSG_CALLSIGN_LEN + 1];
	uint8_t  tx_type;
	uint16_t tx_id;
	uint8_t  result;
	uint8_t  op, error;
} msg_link_event_t;

// Decode one completed KISS frame (msg_kiss_rx_byte's buf/len) from the radio. Returns false (kind NONE) for frames
// that are not the link's (another port, unknown op, wrong length).
bool msg_link_decode_event(const uint8_t *frame, uint16_t len, msg_link_event_t *ev);

#endif

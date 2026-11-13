// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host test: a simulated network of mesh nodes on one shared channel. Each node hears only the nodes in its range
// table; a frame sent at time t arrives at t + AIR_MS. Checks relaying, dedup, ACKs over relays, retries and
// suppression.
#include <stdio.h>
#include <string.h>
#include "messenger/msg_mesh.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define NODES	4
#define AIR_MS	900u
#define QMAX	64

typedef struct {
	int idx;
	msg_mesh_t mesh;
	int delivered;
	char last_text[MSG_TEXT_LEN + 1];
	char last_from[MSG_CALLSIGN_LEN + 1];
	uint8_t last_hops;
	msg_mesh_status_t last_status;
	uint16_t last_status_id;
	int sent;
} node_t;

static node_t nodes[NODES];
static bool hears[NODES][NODES];	// hears[a][b]: a receives what b sends
static uint32_t now;
static uint32_t rng = 12345;
static int drop_next_from = -1;	// drop the next frame this node sends (nobody hears it)

typedef struct { uint8_t pkt[MSG_PKT_WIRE_LEN]; int src; uint32_t at; } air_t;
static air_t air[QMAX];
static int air_n;

static uint32_t fake_rand(void *ctx)
{
	(void)ctx;
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static void on_send(void *ctx, const uint8_t p[MSG_PKT_WIRE_LEN])
{
	node_t *n = ctx;
	n->sent++;
	if (drop_next_from == n->idx) {
		drop_next_from = -1;
		return;
	}
	if (air_n < QMAX) {
		memcpy(air[air_n].pkt, p, MSG_PKT_WIRE_LEN);
		air[air_n].src = n->idx;
		air[air_n].at = now + AIR_MS;
		air_n++;
	}
}

static void on_deliver(void *ctx, const msg_packet_t *p, int8_t rssi, uint8_t hops)
{
	(void)rssi;
	node_t *n = ctx;
	n->delivered++;
	strcpy(n->last_text, (const char *)p->payload);
	strcpy(n->last_from, p->from);
	n->last_hops = hops;
}

static void on_status(void *ctx, uint16_t id, msg_mesh_status_t st)
{
	node_t *n = ctx;
	n->last_status = st;
	n->last_status_id = id;
}

static void setup(const char *const *calls, int count)
{
	memset(nodes, 0, sizeof nodes);
	memset(hears, 0, sizeof hears);
	air_n = 0;
	now = 1000;
	for (int i = 0; i < count; i++) {
		nodes[i].idx = i;
		msg_mesh_cfg_t cfg;
		msg_mesh_default_cfg(&cfg, calls[i]);
		msg_mesh_ops_t ops = { .send = on_send, .deliver = on_deliver, .status = on_status, .rand = fake_rand,
			.ctx = &nodes[i] };
		msg_mesh_init(&nodes[i].mesh, &cfg, &ops);
	}
}

static void link(int a, int b)
{
	hears[a][b] = hears[b][a] = true;
}

// Advance simulated time in 10 ms steps, delivering frames and running timers.
static void run(uint32_t ms, int count)
{
	for (uint32_t t = 0; t < ms; t += 10) {
		now += 10;
		for (int i = 0; i < air_n;) {
			if ((int32_t)(now - air[i].at) >= 0) {
				air_t f = air[i];
				memmove(&air[i], &air[i + 1], (size_t)(air_n - i - 1) * sizeof air[0]);
				air_n--;
				for (int r = 0; r < count; r++) {
					if (r != f.src && hears[r][f.src]) {
						msg_mesh_on_rx(&nodes[r].mesh, f.pkt, -90, now);
					}
				}
			} else {
				i++;
			}
		}
		for (int i = 0; i < count; i++) {
			msg_mesh_tick(&nodes[i].mesh, now);
		}
	}
}

// A - B - C in a line: A and C can't hear each other.
static void test_direct_over_relay(void)
{
	const char *calls[] = { "AAA", "BBB", "CCC" };
	setup(calls, 3);
	link(0, 1);
	link(1, 2);
	uint16_t id;
	CHECK(msg_mesh_send_text(&nodes[0].mesh, "CCC", "hello C", now, &id));
	run(20000, 3);
	CHECK(nodes[2].delivered == 1 && strcmp(nodes[2].last_text, "hello C") == 0);
	CHECK(strcmp(nodes[2].last_from, "AAA") == 0 && nodes[2].last_hops == 1);
	CHECK(nodes[1].delivered == 0);			// B relays but a message to C isn't B's
	CHECK(nodes[0].last_status == MSG_MESH_ST_ACKED && nodes[0].last_status_id == id);
	CHECK(nodes[1].mesh.relayed == 2);		// the TEXT one way and the ACK back
	CHECK(nodes[0].sent == 1);			// no retry was needed
}

// Broadcast in a line: everyone gets it exactly once; A's neighbor ACKs it, the far node doesn't.
static void test_broadcast(void)
{
	const char *calls[] = { "AAA", "BBB", "CCC" };
	setup(calls, 3);
	link(0, 1);
	link(1, 2);
	CHECK(msg_mesh_send_text(&nodes[0].mesh, NULL, "to all", now, NULL));
	run(20000, 3);
	CHECK(nodes[1].delivered == 1 && nodes[2].delivered == 1);
	CHECK(nodes[2].last_hops == 1);
	CHECK(nodes[0].last_status == MSG_MESH_ST_ACKED);	// B's first-hand ACK
	// C heard it relayed (TTL 3 -> 2): no ACK from C, only its own relay hop (2 -> 1).
	CHECK(nodes[2].sent == 1 && nodes[2].mesh.relayed == 1);
}

// The first copy of the TEXT is lost: A retries, C gets it once.
static void test_retry(void)
{
	const char *calls[] = { "AAA", "BBB" };
	setup(calls, 2);
	link(0, 1);
	drop_next_from = 0;
	CHECK(msg_mesh_send_text(&nodes[0].mesh, "BBB", "again", now, NULL));
	run(30000, 2);
	CHECK(nodes[1].delivered == 1);
	CHECK(nodes[0].sent == 2);
	CHECK(nodes[0].last_status == MSG_MESH_ST_ACKED);
}

// The ACK is lost: A retries, B dedups (no second delivery) but ACKs again.
static void test_lost_ack(void)
{
	const char *calls[] = { "AAA", "BBB" };
	setup(calls, 2);
	link(0, 1);
	CHECK(msg_mesh_send_text(&nodes[0].mesh, "BBB", "ack lost", now, NULL));
	run(500, 2);
	drop_next_from = 1;				// B's first ACK goes nowhere
	run(30000, 2);
	CHECK(nodes[1].delivered == 1);
	CHECK(nodes[1].mesh.duplicates == 1);
	CHECK(nodes[0].last_status == MSG_MESH_ST_ACKED);
}

// Nobody there: a direct message ends FAILED after 1 + retries tries.
static void test_failed(void)
{
	const char *calls[] = { "AAA", "BBB" };
	setup(calls, 2);
	CHECK(msg_mesh_send_text(&nodes[0].mesh, "BBB", "anyone?", now, NULL));
	run(60000, 2);
	CHECK(nodes[0].sent == 3 && nodes[0].last_status == MSG_MESH_ST_FAILED);
}

// A diamond A - {B, C} - D: B and C both hear A's broadcast; usually whichever relays first suppresses the other.
// D must deliver exactly once every time; over many random seeds, suppression must save a burst most of the time.
static void test_suppression(void)
{
	const char *calls[] = { "AAA", "BBB", "CCC", "DDD" };
	int saved = 0;
	for (uint32_t seed = 1; seed <= 40; seed++) {
		rng = seed * 7919u;
		setup(calls, 4);
		link(0, 1);
		link(0, 2);
		link(1, 2);
		link(1, 3);
		link(2, 3);
		nodes[0].mesh.cfg.ttl = 2;		// one relay hop is enough here
		nodes[0].mesh.cfg.ack_broadcast = false;
		nodes[1].mesh.cfg.ack_broadcast = false;
		nodes[2].mesh.cfg.ack_broadcast = false;
		nodes[3].mesh.cfg.ack_broadcast = false;
		CHECK(msg_mesh_send_text(&nodes[0].mesh, NULL, "diamond", now, NULL));
		run(20000, 4);
		CHECK(nodes[3].delivered == 1);
		if (nodes[1].mesh.suppressed + nodes[2].mesh.suppressed >= 1) {
			saved++;
		}
	}
	printf("suppression saved a burst in %d of 40 runs\n", saved);
	CHECK(saved >= 20);
}

// Malformed or own frames are ignored.
static void test_ignore(void)
{
	const char *calls[] = { "AAA" };
	setup(calls, 1);
	uint8_t p[MSG_PKT_WIRE_LEN];
	msg_packet_build_text(p, sizeof p, 7, "AAA", "me", 3);
	msg_mesh_on_rx(&nodes[0].mesh, p, -50, now);
	CHECK(nodes[0].delivered == 0 && nodes[0].sent == 0);
	msg_packet_build_text(p, sizeof p, 8, "ZZZ", "x", 3);
	p[40] ^= 1;
	msg_mesh_on_rx(&nodes[0].mesh, p, -50, now);
	CHECK(nodes[0].delivered == 0);
}

int main(void)
{
	test_direct_over_relay();
	test_broadcast();
	test_retry();
	test_lost_ack();
	test_failed();
	test_suppression();
	test_ignore();
	if (failures) {
		printf("msg_mesh test: %d failure(s)\n", failures);
		return 1;
	}
	printf("msg_mesh test: OK (direct over a relay with ACK back, broadcast, retry, lost ACK, failure, "
		"relay suppression, ignored frames)\n");
	return 0;
}

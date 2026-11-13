// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Store-and-forward mesh over GGM2 (msg_mesh.h).
#include <string.h>
#include "messenger/msg_mesh.h"

void msg_mesh_default_cfg(msg_mesh_cfg_t *cfg, const char *call)
{
	memset(cfg, 0, sizeof *cfg);
	for (unsigned i = 0; call != NULL && call[i] != '\0' && i < MSG_CALLSIGN_LEN; i++) {
		cfg->call[i] = call[i];
	}
	cfg->ttl = 3;
	cfg->relay = true;
	cfg->ack_broadcast = true;
	cfg->retries = 2;
	cfg->relay_min_ms = 200;
	cfg->relay_slot_ms = 1200;	// one GGM2 burst on air is ~0.9 s
	cfg->relay_slots = 4;
	// One GGM2 burst is about 0.9 s on air (16-byte preamble + 100 bytes at 1200 baud) plus keying; a hop costs a
	// relay delay and a burst each way.
	cfg->ack_base_ms = 3000;
	cfg->ack_hop_ms = 4500;
}

void msg_mesh_init(msg_mesh_t *m, const msg_mesh_cfg_t *cfg, const msg_mesh_ops_t *ops)
{
	memset(m, 0, sizeof *m);
	m->cfg = *cfg;
	m->ops = *ops;
	m->next_id = (uint16_t)(ops->rand(ops->ctx) | 1u);
}

void msg_mesh_set_call(msg_mesh_t *m, const char *call)
{
	memset(m->cfg.call, 0, sizeof m->cfg.call);
	for (unsigned i = 0; call != NULL && call[i] != '\0' && i < MSG_CALLSIGN_LEN; i++) {
		m->cfg.call[i] = call[i];
	}
}

static bool same_call(const char *a, const char *b)
{
	return strncmp(a, b, MSG_CALLSIGN_LEN) == 0;
}

static bool is_all(const char *to)
{
	return to == NULL || to[0] == '\0' || same_call(to, MSG_PKT_TO_ALL);
}

static uint32_t rand_between(msg_mesh_t *m, uint32_t lo, uint32_t hi)
{
	if (hi <= lo) {
		return lo;
	}
	return lo + m->ops.rand(m->ops.ctx) % (hi - lo + 1u);
}

static bool due(uint32_t now, uint32_t at)
{
	return (int32_t)(now - at) >= 0;
}

// ---- seen table ---------------------------------------------------------------------------------------------------
static msg_mesh_seen_t *seen_find(msg_mesh_t *m, const char *from, uint16_t id, uint8_t type, uint32_t now)
{
	for (unsigned i = 0; i < MSG_MESH_SEEN_MAX; i++) {
		msg_mesh_seen_t *s = &m->seen[i];
		if (s->used && s->id == id && s->type == type &&
		    memcmp(s->key_from, from, MSG_CALLSIGN_LEN) == 0) {
			if ((uint32_t)(now - s->at) > MSG_MESH_SEEN_MS) {
				s->used = false;	// expired: treat as new
				return NULL;
			}
			return s;
		}
	}
	return NULL;
}

static void seen_add(msg_mesh_t *m, const char *from, uint16_t id, uint8_t type, uint32_t now)
{
	msg_mesh_seen_t *s = &m->seen[m->seen_next];
	m->seen_next = (uint8_t)((m->seen_next + 1u) % MSG_MESH_SEEN_MAX);
	memset(s, 0, sizeof *s);
	for (unsigned i = 0; i < MSG_CALLSIGN_LEN && from[i] != '\0'; i++) {
		s->key_from[i] = (uint8_t)from[i];
	}
	s->id = id;
	s->type = type;
	s->at = now;
	s->used = true;
}

// ---- relays -------------------------------------------------------------------------------------------------------
static void relay_schedule(msg_mesh_t *m, const uint8_t packet[MSG_PKT_WIRE_LEN], uint8_t ttl_remain, uint32_t now)
{
	for (unsigned i = 0; i < MSG_MESH_RELAY_MAX; i++) {
		msg_mesh_relay_t *r = &m->relay[i];
		if (!r->used) {
			r->used = true;
			memcpy(r->packet, packet, MSG_PKT_WIRE_LEN);
			msg_packet_set_ttl_remain(r->packet, (uint8_t)(ttl_remain - 1u));
			uint32_t slots = m->cfg.relay_slots ? m->cfg.relay_slots : 1u;
			uint32_t slot = m->ops.rand(m->ops.ctx) % slots;
			r->due = now + m->cfg.relay_min_ms + slot * m->cfg.relay_slot_ms + rand_between(m, 0, 99);
			return;
		}
	}
	// Relay table full: this copy is dropped; a neighbor will usually carry it.
}

// Another node relayed the frame we were about to relay: ours adds nothing for the shared neighborhood.
static void relay_cancel(msg_mesh_t *m, const msg_packet_t *p)
{
	for (unsigned i = 0; i < MSG_MESH_RELAY_MAX; i++) {
		msg_mesh_relay_t *r = &m->relay[i];
		if (!r->used) {
			continue;
		}
		msg_packet_t q;
		if (msg_packet_parse(r->packet, MSG_PKT_WIRE_LEN, &q) && q.id == p->id && q.type == p->type &&
		    same_call(q.from, p->from)) {
			r->used = false;
			m->suppressed++;
		}
	}
}

// ---- own messages -------------------------------------------------------------------------------------------------
static uint32_t ack_window(const msg_mesh_t *m, uint8_t ttl)
{
	return (uint32_t)m->cfg.ack_base_ms + (uint32_t)m->cfg.ack_hop_ms * ttl;
}

static void send_ack(msg_mesh_t *m, const msg_packet_t *p, uint8_t ttl)
{
	uint8_t pkt[MSG_PKT_WIRE_LEN];
	if (msg_packet_build_ack_ttl(pkt, sizeof pkt, p->id, m->cfg.call, p->from, ttl) == MSG_PKT_WIRE_LEN) {
		m->ops.send(m->ops.ctx, pkt);
	}
}

bool msg_mesh_send_text(msg_mesh_t *m, const char *to, const char *text, uint32_t now_ms, uint16_t *id_out)
{
	if (text == NULL || text[0] == '\0') {
		return false;
	}
	msg_mesh_pending_t *slot = NULL;
	for (unsigned i = 0; i < MSG_MESH_PENDING_MAX; i++) {
		if (!m->pending[i].used) {
			slot = &m->pending[i];
			break;
		}
	}
	if (slot == NULL) {
		return false;
	}
	uint16_t id = m->next_id++;
	if (m->next_id == 0) {
		m->next_id = 1;
	}
	uint8_t ttl = m->cfg.ttl ? m->cfg.ttl : 1u;
	if (msg_packet_build_text_to(slot->packet, sizeof slot->packet, id, m->cfg.call, is_all(to) ? MSG_PKT_TO_ALL : to,
		text, ttl) != MSG_PKT_WIRE_LEN) {
		return false;
	}
	slot->used = true;
	slot->to_all = is_all(to);
	slot->id = id;
	slot->tries = 1;
	slot->deadline = now_ms + ack_window(m, ttl);
	seen_add(m, m->cfg.call, id, MSG_PKT_TYPE_TEXT, now_ms);
	m->ops.send(m->ops.ctx, slot->packet);
	if (m->ops.status != NULL) {
		m->ops.status(m->ops.ctx, id, MSG_MESH_ST_SENT);
	}
	if (id_out != NULL) {
		*id_out = id;
	}
	return true;
}

static void pending_acked(msg_mesh_t *m, uint16_t id)
{
	for (unsigned i = 0; i < MSG_MESH_PENDING_MAX; i++) {
		msg_mesh_pending_t *pd = &m->pending[i];
		if (pd->used && pd->id == id) {
			pd->used = false;
			if (m->ops.status != NULL) {
				m->ops.status(m->ops.ctx, id, MSG_MESH_ST_ACKED);
			}
			return;
		}
	}
}

// ---- receive ------------------------------------------------------------------------------------------------------
void msg_mesh_on_rx(msg_mesh_t *m, const uint8_t packet[MSG_PKT_WIRE_LEN], int8_t rssi, uint32_t now_ms)
{
	msg_packet_t p;
	if (!msg_packet_parse(packet, MSG_PKT_WIRE_LEN, &p)) {
		return;
	}
	if (same_call(p.from, m->cfg.call)) {
		relay_cancel(m, &p);		// our own frame echoed by a relay: nothing to do
		return;
	}
	if (m->ops.heard != NULL) {
		m->ops.heard(m->ops.ctx, &p, rssi);
	}
	uint8_t hops = (p.ttl_init >= p.ttl_remain) ? (uint8_t)(p.ttl_init - p.ttl_remain) : 0u;
	bool to_me = same_call(p.to, m->cfg.call);
	bool to_all = same_call(p.to, MSG_PKT_TO_ALL);

	if (seen_find(m, p.from, p.id, p.type, now_ms) != NULL) {
		m->duplicates++;
		relay_cancel(m, &p);
		if (p.type == MSG_PKT_TYPE_TEXT && to_me) {
			send_ack(m, &p, (uint8_t)(hops + 1u));	// the sender is retrying: our ACK may have been lost
		}
		return;
	}
	seen_add(m, p.from, p.id, p.type, now_ms);

	switch (p.type) {
	case MSG_PKT_TYPE_TEXT:
		if (to_me || to_all) {
			m->ops.deliver(m->ops.ctx, &p, rssi, hops);
		}
		if (to_me) {
			send_ack(m, &p, (uint8_t)(hops + 1u));
		} else if (to_all && hops == 0 && m->cfg.ack_broadcast) {
			send_ack(m, &p, 1);
		}
		if (!to_me && m->cfg.relay && p.ttl_remain > 1u) {
			relay_schedule(m, packet, p.ttl_remain, now_ms);
			m->relayed++;
		}
		break;
	case MSG_PKT_TYPE_ACK:
		if (to_me) {
			pending_acked(m, p.id);
		} else if (m->cfg.relay && p.ttl_remain > 1u) {
			relay_schedule(m, packet, p.ttl_remain, now_ms);
			m->relayed++;
		}
		break;
	default:
		break;				// PING/PONG/WAKE: neighbor-only
	}
}

void msg_mesh_tick(msg_mesh_t *m, uint32_t now_ms)
{
	for (unsigned i = 0; i < MSG_MESH_RELAY_MAX; i++) {
		msg_mesh_relay_t *r = &m->relay[i];
		if (r->used && due(now_ms, r->due)) {
			r->used = false;
			m->ops.send(m->ops.ctx, r->packet);
		}
	}
	for (unsigned i = 0; i < MSG_MESH_PENDING_MAX; i++) {
		msg_mesh_pending_t *pd = &m->pending[i];
		if (!pd->used || !due(now_ms, pd->deadline)) {
			continue;
		}
		if (pd->to_all) {
			pd->used = false;
			if (m->ops.status != NULL) {
				m->ops.status(m->ops.ctx, pd->id, MSG_MESH_ST_NO_ACK);
			}
		} else if (pd->tries <= m->cfg.retries) {
			pd->tries++;
			pd->deadline = now_ms + ack_window(m, pd->packet[9]) + rand_between(m, 200, 800);
			m->ops.send(m->ops.ctx, pd->packet);	// same id: receivers dedup it and ACK again
			if (m->ops.status != NULL) {
				m->ops.status(m->ops.ctx, pd->id, MSG_MESH_ST_SENT);
			}
		} else {
			pd->used = false;
			if (m->ops.status != NULL) {
				m->ops.status(m->ops.ctx, pd->id, MSG_MESH_ST_FAILED);
			}
		}
	}
}

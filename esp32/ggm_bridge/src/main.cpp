// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GGM2 mesh host on an ESP32 wired to a radio's UART (docs/bridge-protocol.md). The radio's BK4819 does the FSK; this
// firmware runs the mesh (msg_mesh), keeps the message log, and offers it to a phone over a WiFi access point
// (http://192.168.4.1) and to a PC over the USB serial console.
//
// Serial console (115200, USB): "call SV4QXF", "send ALL hello", "send SV4ABC hi there", "status", "wifi on|off".
//
// GGM_SIM_RADIO=1 builds a bench version with no radio: the "air" is ESP-NOW broadcast between boards on WiFi
// channel 1, each frame tagged with the sending node's callsign. Console "deaf CALL" / "hear CALL" makes this node
// ignore / hear one station again, to build relay topologies on a desk (A deaf to C and C deaf to A = A-B-C line).
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_random.h>
#ifndef GGM_SIM_RADIO
#define GGM_SIM_RADIO 0
#endif
#if GGM_SIM_RADIO
#include <esp_now.h>
#include <esp_wifi.h>
#endif

extern "C" {
#include "messenger/msg_packet.h"
#include "messenger/msg_kiss.h"
#include "messenger/msg_link.h"
#include "messenger/msg_mesh.h"
}

#ifndef GGM_LINK_BAUD
#define GGM_LINK_BAUD 38400
#endif
#ifndef GGM_LINK_TX
#define GGM_LINK_TX 21
#endif
#ifndef GGM_LINK_RX
#define GGM_LINK_RX 20
#endif

static HardwareSerial radio(1);
#if GGM_SIM_RADIO
#define SIM_AIR_MS 900			// one GGM2 burst on air: the next frame waits this long, like the real radio
static void sim_send(const uint8_t p[MSG_PKT_WIRE_LEN]);
static uint32_t sim_air_free_ms;
#endif
static WebServer web(80);
static Preferences prefs;

// ---- settings -----------------------------------------------------------------------------------------------------
static char own_call[MSG_CALLSIGN_LEN + 1] = "NOCALL";
static char ap_pass[33] = "ggm12345";
static bool wifi_on = true;

static void load_settings()
{
	prefs.begin("ggm", false);
	String c = prefs.getString("call", "NOCALL");
	strlcpy(own_call, c.c_str(), sizeof own_call);
	String p = prefs.getString("pass", "ggm12345");
	strlcpy(ap_pass, p.c_str(), sizeof ap_pass);
	wifi_on = prefs.getBool("wifi", true);
}

static bool valid_call(const char *c)
{
	size_t n = strlen(c);
	if (n == 0 || n > MSG_CALLSIGN_LEN) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (!isalnum((unsigned char)c[i]) && c[i] != '-' && c[i] != '/') {
			return false;
		}
	}
	return true;
}

// ---- message log --------------------------------------------------------------------------------------------------
struct LogMsg {
	uint32_t t;
	bool out;
	char from[MSG_CALLSIGN_LEN + 1];
	char to[MSG_CALLSIGN_LEN + 1];
	char text[MSG_TEXT_LEN + 1];
	uint16_t id;
	uint8_t status;			// msg_mesh_status_t for our own; 0xFF = received
	int8_t rssi;
	uint8_t hops;
};
#define LOG_MAX 40
static LogMsg logs[LOG_MAX];
static uint8_t log_next, log_count;
static uint32_t log_seq;		// bumps on every change so the page can poll cheaply

static LogMsg *log_add()
{
	LogMsg *m = &logs[log_next];
	log_next = (uint8_t)((log_next + 1) % LOG_MAX);
	if (log_count < LOG_MAX) {
		log_count++;
	}
	memset(m, 0, sizeof *m);
	m->t = millis();
	log_seq++;
	return m;
}

struct Heard {
	char call[MSG_CALLSIGN_LEN + 1];
	int8_t rssi;
	uint32_t t;
	uint8_t type;
};
#define HEARD_MAX 10
static Heard heard[HEARD_MAX];

static void heard_add(const char *call, int8_t rssi, uint8_t type)
{
	int slot = -1, oldest = 0;
	for (int i = 0; i < HEARD_MAX; i++) {
		if (heard[i].call[0] && strncmp(heard[i].call, call, MSG_CALLSIGN_LEN) == 0) {
			slot = i;
			break;
		}
		if (!heard[i].call[0]) {
			if (slot < 0) {
				slot = i;
			}
		} else if (heard[i].t < heard[oldest].t) {
			oldest = i;
		}
	}
	if (slot < 0) {
		slot = oldest;
	}
	strlcpy(heard[slot].call, call, sizeof heard[slot].call);
	heard[slot].rssi = rssi;
	heard[slot].t = millis();
	heard[slot].type = type;
}

// ---- link to the radio --------------------------------------------------------------------------------------------
static msg_kiss_rx_t krx;
static bool radio_attached;
static uint8_t radio_caps;
static char radio_call[MSG_CALLSIGN_LEN + 1];
static uint32_t last_hello_ms, hello_sent_ms, last_tx_ms;
static bool hello_outstanding;
static uint32_t link_errors, tx_failures;

static void link_write(const uint8_t *b, size_t n)
{
	if (n > 0) {
		radio.write(b, n);
		last_tx_ms = millis();
	}
}

static void send_hello()
{
	uint8_t f[16];
	link_write(f, msg_link_encode_hello(f, sizeof f));
	hello_sent_ms = last_hello_ms = millis();
	hello_outstanding = true;
}

static void send_call_to_radio()
{
	uint8_t f[32];
	link_write(f, msg_link_encode_set_call(f, sizeof f, own_call));
}

// Transmit queue: one packet in flight at the radio, the rest wait for its TX_RESULT.
#define TXQ_MAX 8
static uint8_t txq[TXQ_MAX][MSG_PKT_WIRE_LEN];
static uint8_t txq_head, txq_count;
static bool tx_in_flight;
static uint32_t tx_since, tx_next_try;
static uint8_t tx_busy_retries;

static void txq_push(const uint8_t p[MSG_PKT_WIRE_LEN])
{
	if (txq_count >= TXQ_MAX) {
		tx_failures++;
		return;
	}
	memcpy(txq[(txq_head + txq_count) % TXQ_MAX], p, MSG_PKT_WIRE_LEN);
	txq_count++;
}

static void txq_pop()
{
	if (txq_count) {
		txq_head = (uint8_t)((txq_head + 1) % TXQ_MAX);
		txq_count--;
	}
	tx_in_flight = false;
	tx_busy_retries = 0;
}

static void txq_pump(uint32_t now)
{
#if GGM_SIM_RADIO
	if (txq_count && (int32_t)(now - sim_air_free_ms) >= 0) {
		sim_send(txq[txq_head]);
		sim_air_free_ms = now + SIM_AIR_MS;
		txq_pop();
	}
	return;
#endif
	if (tx_in_flight) {
		if (now - tx_since > 5000) {		// no result: the radio lost it
			tx_failures++;
			txq_pop();
		}
		return;
	}
	if (!txq_count || !radio_attached || (int32_t)(now - tx_next_try) < 0) {
		return;
	}
	uint8_t f[MSG_LINK_FRAME_MAX];
	link_write(f, msg_link_encode_tx(f, sizeof f, txq[txq_head]));
	tx_in_flight = true;
	tx_since = now;
}

static void on_tx_result(const msg_link_event_t &ev, uint32_t now)
{
	if (!tx_in_flight || !txq_count) {
		return;
	}
	const uint8_t *p = txq[txq_head];
	uint16_t id = (uint16_t)(p[7] | (p[8] << 8));
	if (ev.tx_type != p[5] || ev.tx_id != id) {
		return;					// a result for something else
	}
	switch (ev.result) {
	case MSG_LINK_TX_QUEUE_FULL:
		tx_in_flight = false;
		tx_next_try = now + 300;
		break;
	case MSG_LINK_TX_BUSY:
		if (tx_busy_retries++ < 3) {
			tx_in_flight = false;
			tx_next_try = now + 500 + esp_random() % 1500;
			break;
		}
		tx_failures++;
		txq_pop();
		break;
	case MSG_LINK_TX_OK:
		txq_pop();
		break;
	default:
		tx_failures++;
		txq_pop();
		break;
	}
}

// ---- simulated air (GGM_SIM_RADIO) --------------------------------------------------------------------------------
#if GGM_SIM_RADIO
#define SIM_RXQ 8
#define SIM_DEAF_MAX 6
struct SimFrame {
	char tx_call[MSG_CALLSIGN_LEN];		// who keyed up (relays included), for the deaf list
	uint8_t packet[MSG_PKT_WIRE_LEN];
};
static SimFrame sim_rxq[SIM_RXQ];
static int8_t sim_rxq_rssi[SIM_RXQ];
static volatile uint8_t sim_rx_head, sim_rx_tail;	// WiFi task produces, loop() consumes
static char sim_deaf[SIM_DEAF_MAX][MSG_CALLSIGN_LEN + 1];
static const uint8_t SIM_BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void sim_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	if (len != (int)sizeof(SimFrame)) {
		return;
	}
	uint8_t next = (uint8_t)((sim_rx_head + 1) % SIM_RXQ);
	if (next == sim_rx_tail) {
		return;					// full: lost, like a collision
	}
	memcpy(&sim_rxq[sim_rx_head], data, sizeof(SimFrame));
	sim_rxq_rssi[sim_rx_head] = (int8_t)(info->rx_ctrl ? info->rx_ctrl->rssi : -50);
	sim_rx_head = next;
}

static bool sim_is_deaf(const char *call)
{
	for (int i = 0; i < SIM_DEAF_MAX; i++) {
		if (sim_deaf[i][0] && strncmp(sim_deaf[i], call, MSG_CALLSIGN_LEN) == 0) {
			return true;
		}
	}
	return false;
}

static void sim_set_deaf(const char *call, bool deaf)
{
	for (int i = 0; i < SIM_DEAF_MAX; i++) {
		if (sim_deaf[i][0] && strncmp(sim_deaf[i], call, MSG_CALLSIGN_LEN) == 0) {
			if (!deaf) {
				sim_deaf[i][0] = 0;
			}
			return;
		}
	}
	for (int i = 0; deaf && i < SIM_DEAF_MAX; i++) {
		if (!sim_deaf[i][0]) {
			strlcpy(sim_deaf[i], call, sizeof sim_deaf[i]);
			return;
		}
	}
}

static void sim_start()
{
	if (esp_now_init() != ESP_OK) {
		Serial.println("ESP-NOW init failed");
		return;
	}
	esp_now_register_recv_cb(sim_recv);
	esp_now_peer_info_t peer = {};
	memcpy(peer.peer_addr, SIM_BCAST, 6);
	peer.channel = 0;			// the AP's channel
	peer.ifidx = WIFI_IF_AP;
	peer.encrypt = false;
	esp_now_add_peer(&peer);
	radio_attached = true;
	radio_caps = MSG_LINK_CAP_TX;
	strlcpy(radio_call, "SIM", sizeof radio_call);
}

static void sim_send(const uint8_t p[MSG_PKT_WIRE_LEN])
{
	SimFrame f;
	memset(&f, 0, sizeof f);
	memcpy(f.tx_call, own_call, strnlen(own_call, MSG_CALLSIGN_LEN));
	memcpy(f.packet, p, MSG_PKT_WIRE_LEN);
	esp_now_send(SIM_BCAST, (const uint8_t *)&f, sizeof f);
}
#endif

// ---- mesh ---------------------------------------------------------------------------------------------------------
static msg_mesh_t mesh;

static void mesh_send(void *, const uint8_t p[MSG_PKT_WIRE_LEN])
{
	txq_push(p);
}

static void mesh_deliver(void *, const msg_packet_t *p, int8_t rssi, uint8_t hops)
{
	LogMsg *m = log_add();
	m->out = false;
	strlcpy(m->from, p->from, sizeof m->from);
	strlcpy(m->to, p->to, sizeof m->to);
	memcpy(m->text, p->payload, p->payload_len);
	m->text[p->payload_len] = 0;
	m->id = p->id;
	m->status = 0xFF;
	m->rssi = rssi;
	m->hops = hops;
	Serial.printf("RX %s>%s (%d dBm, %u hop): %s\n", m->from, m->to, rssi, hops, m->text);
}

static void mesh_status(void *, uint16_t id, msg_mesh_status_t st)
{
	for (int i = 0; i < LOG_MAX; i++) {
		if (logs[i].out && logs[i].id == id) {
			logs[i].status = (uint8_t)st;
			log_seq++;
		}
	}
	static const char *const names[] = { "sent", "ACKED", "no ack", "FAILED" };
	Serial.printf("TX #%u: %s\n", id, names[st & 3]);
}

static void mesh_heard(void *, const msg_packet_t *p, int8_t rssi)
{
	if (p->ttl_init == p->ttl_remain) {		// first-hand only: relayed frames don't say who is in range
		heard_add(p->from, rssi, p->type);
	}
}

static uint32_t mesh_rand(void *)
{
	return esp_random();
}

static void mesh_setup()
{
	msg_mesh_cfg_t cfg;
	msg_mesh_default_cfg(&cfg, own_call);
	msg_mesh_ops_t ops = {};
	ops.send = mesh_send;
	ops.deliver = mesh_deliver;
	ops.status = mesh_status;
	ops.heard = mesh_heard;
	ops.rand = mesh_rand;
	msg_mesh_init(&mesh, &cfg, &ops);
}

static bool send_text(const char *to, const char *text, uint16_t *id_out)
{
	uint16_t id = 0;
	if (!msg_mesh_send_text(&mesh, to, text, millis(), &id)) {
		return false;
	}
	LogMsg *m = log_add();
	m->out = true;
	strlcpy(m->from, own_call, sizeof m->from);
	strlcpy(m->to, (to && *to) ? to : MSG_PKT_TO_ALL, sizeof m->to);
	strlcpy(m->text, text, sizeof m->text);
	m->id = id;
	m->status = MSG_MESH_ST_SENT;
	if (id_out) {
		*id_out = id;
	}
	return true;
}

static void set_call(const char *c)
{
	strlcpy(own_call, c, sizeof own_call);
	for (char *p = own_call; *p; p++) {
		*p = (char)toupper((unsigned char)*p);
	}
	prefs.putString("call", own_call);
	msg_mesh_set_call(&mesh, own_call);
	if (radio_attached) {
		send_call_to_radio();
	}
}

static void link_poll(uint32_t now)
{
#if GGM_SIM_RADIO
	while (sim_rx_tail != sim_rx_head) {
		uint8_t i = sim_rx_tail;
		char tx_call[MSG_CALLSIGN_LEN + 1] = {};
		memcpy(tx_call, sim_rxq[i].tx_call, MSG_CALLSIGN_LEN);
		if (!sim_is_deaf(tx_call)) {
			msg_mesh_on_rx(&mesh, sim_rxq[i].packet, sim_rxq_rssi[i], now);
		}
		sim_rx_tail = (uint8_t)((i + 1) % SIM_RXQ);
	}
	return;
#endif
	while (radio.available()) {
		uint16_t n = msg_kiss_rx_byte(&krx, (uint8_t)radio.read());
		if (!n) {
			continue;
		}
		msg_link_event_t ev;
		if (!msg_link_decode_event(krx.buf, n, &ev)) {
			continue;
		}
		switch (ev.kind) {
		case MSG_LINK_EVENT_HELLO:
			if (!radio_attached) {
				Serial.printf("radio attached: %s, link v%u, caps %02X\n", ev.call, ev.version, ev.caps);
			}
			radio_attached = true;
			hello_outstanding = false;
			radio_caps = ev.caps;
			strlcpy(radio_call, ev.call, sizeof radio_call);
			if (strcmp(radio_call, own_call) != 0 && strcmp(own_call, "NOCALL") != 0) {
				send_call_to_radio();
			}
			break;
		case MSG_LINK_EVENT_RX:
			msg_mesh_on_rx(&mesh, ev.packet, ev.rssi, now);
			break;
		case MSG_LINK_EVENT_TX_RESULT:
			on_tx_result(ev, now);
			break;
		case MSG_LINK_EVENT_ERROR:
			link_errors++;
			if (tx_in_flight && ev.op == MSG_LINK_CMD_DATA) {
				txq_pop();		// the radio refused that packet: don't send it again
			}
			break;
		default:
			break;
		}
	}
	// Attach / stay attached: HELLO until answered, then every 30 s; keepalive in between.
	if (hello_outstanding && now - hello_sent_ms > 3000) {
		if (radio_attached) {
			Serial.println("radio lost (no HELLO reply)");
		}
		radio_attached = false;
		hello_outstanding = false;
	}
	if (!hello_outstanding && (!radio_attached ? now - last_hello_ms > 3000 : now - last_hello_ms > 30000)) {
		send_hello();
	} else if (radio_attached && now - last_tx_ms > MSG_LINK_KEEPALIVE_MS) {
		uint8_t f[8];
		link_write(f, msg_link_encode_keepalive(f, sizeof f));
	}
}

// ---- web UI -------------------------------------------------------------------------------------------------------
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>GGM mesh</title><style>
body{font:15px system-ui,sans-serif;margin:0;background:#101418;color:#e8eaed}
header{padding:10px 14px;background:#1b2229;display:flex;justify-content:space-between;align-items:center}
#st{font-size:12px;opacity:.8}#log{padding:8px 10px 120px}
.m{max-width:85%;margin:6px 0;padding:7px 10px;border-radius:10px;background:#24303a;word-wrap:break-word}
.o{margin-left:auto;background:#1f4d3a}.h{font-size:11px;opacity:.7;margin-bottom:2px}
form{position:fixed;bottom:0;left:0;right:0;background:#1b2229;padding:8px;display:flex;gap:6px}
input{background:#101418;color:#e8eaed;border:1px solid #3a4652;border-radius:8px;padding:9px}
#to{width:28%}#tx{flex:1}button{background:#2e7d5b;color:#fff;border:0;border-radius:8px;padding:0 14px}
details{padding:4px 14px;font-size:13px}</style></head><body>
<header><b id="me">GGM</b><span id="st">…</span></header>
<details><summary>Heard / settings</summary><div id="heard"></div>
<p><input id="call" placeholder="callsign" maxlength="8"> <button onclick="setCall()">Set call</button></p></details>
<div id="log"></div>
<form onsubmit="send();return false"><input id="to" value="ALL" maxlength="8">
<input id="tx" maxlength="64" placeholder="message (64 max)" autocomplete="off"><button>Send</button></form>
<script>
let seq=-1;const ST=['sending','delivered','no ack','failed'];
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function poll(){try{const r=await fetch('/api/state');const s=await r.json();
me.textContent=s.call;st.textContent=(s.radio.attached?'radio '+s.radio.call:'no radio')+' · relayed '+s.relayed;
heard.innerHTML=s.heard.map(h=>esc(h.call)+' '+h.rssi+' dBm, '+h.ago+' s ago').join('<br>')||'nobody yet';
if(s.seq!==seq){seq=s.seq;const atEnd=innerHeight+scrollY>=document.body.scrollHeight-40;
log.innerHTML=s.msgs.map(m=>'<div class="m '+(m.out?'o':'')+'"><div class="h">'+esc(m.from)+' → '+esc(m.to)+
(m.out?' · '+ST[m.status]:' · '+m.rssi+' dBm'+(m.hops?' · '+m.hops+' hop':''))+'</div>'+esc(m.text)+'</div>').join('');
if(atEnd)scrollTo(0,document.body.scrollHeight)}}catch(e){st.textContent='offline'}}
async function send(){const t=tx.value.trim();if(!t)return;const b=new URLSearchParams({to:to.value.trim()||'ALL',text:t});
const r=await fetch('/api/send',{method:'POST',body:b});if(r.ok)tx.value='';else alert(await r.text());poll()}
async function setCall(){const b=new URLSearchParams({call:call.value.trim()});
const r=await fetch('/api/call',{method:'POST',body:b});if(!r.ok)alert(await r.text());poll()}
setInterval(poll,1500);poll();</script></body></html>)HTML";

static void json_str(String &out, const char *s)
{
	out += '"';
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		} else if (c < 0x20 || c >= 0x7F) {
			char b[8];
			snprintf(b, sizeof b, "\\u%04x", c);	// GGM2 text is 8-bit; show non-ASCII bytes as their code
			out += b;
		} else {
			out += (char)c;
		}
	}
	out += '"';
}

static void api_state()
{
	uint32_t now = millis();
	String o;
	o.reserve(4096);
	o += "{\"call\":";
	json_str(o, own_call);
	o += ",\"seq\":";
	o += log_seq;
	o += ",\"relayed\":";
	o += mesh.relayed;
	o += ",\"radio\":{\"attached\":";
	o += radio_attached ? "true" : "false";
	o += ",\"call\":";
	json_str(o, radio_call);
	o += ",\"caps\":";
	o += radio_caps;
	o += ",\"queue\":";
	o += txq_count;
	o += ",\"tx_failures\":";
	o += tx_failures;
	o += "},\"heard\":[";
	bool first = true;
	for (int i = 0; i < HEARD_MAX; i++) {
		if (!heard[i].call[0]) {
			continue;
		}
		o += first ? "" : ",";
		first = false;
		o += "{\"call\":";
		json_str(o, heard[i].call);
		o += ",\"rssi\":";
		o += heard[i].rssi;
		o += ",\"ago\":";
		o += (now - heard[i].t) / 1000;
		o += '}';
	}
	o += "],\"msgs\":[";
	for (int k = 0; k < log_count; k++) {
		const LogMsg &m = logs[(log_next + LOG_MAX - log_count + k) % LOG_MAX];
		o += k ? "," : "";
		o += "{\"out\":";
		o += m.out ? "true" : "false";
		o += ",\"from\":";
		json_str(o, m.from);
		o += ",\"to\":";
		json_str(o, m.to);
		o += ",\"text\":";
		json_str(o, m.text);
		o += ",\"id\":";
		o += m.id;
		o += ",\"status\":";
		o += m.status == 0xFF ? 0 : m.status;
		o += ",\"rssi\":";
		o += m.rssi;
		o += ",\"hops\":";
		o += m.hops;
		o += '}';
	}
	o += "]}";
	web.send(200, "application/json", o);
}

static void api_send()
{
	String to = web.arg("to"), text = web.arg("text");
	to.trim();
	to.toUpperCase();
	if (to.length() == 0) {
		to = MSG_PKT_TO_ALL;
	}
	if (text.length() == 0 || text.length() > MSG_TEXT_LEN || !valid_call(to.c_str())) {
		web.send(400, "text/plain", "need to (1-8 chars) and text (1-64 bytes)");
		return;
	}
	if (strcmp(own_call, "NOCALL") == 0) {
		web.send(400, "text/plain", "set your callsign first");
		return;
	}
	uint16_t id;
	if (!send_text(to.c_str(), text.c_str(), &id)) {
		web.send(503, "text/plain", "too many messages waiting for an ACK");
		return;
	}
	web.send(200, "application/json", String("{\"id\":") + id + "}");
}

static void api_call()
{
	String c = web.arg("call");
	c.trim();
	if (!valid_call(c.c_str())) {
		web.send(400, "text/plain", "callsign: 1-8 letters/digits");
		return;
	}
	set_call(c.c_str());
	web.send(200, "text/plain", "ok");
}

static void wifi_start()
{
	WiFi.mode(GGM_SIM_RADIO ? WIFI_AP_STA : WIFI_AP);
	String ssid = String("GGM-") + own_call;
	WiFi.softAP(ssid.c_str(), ap_pass, 1);		// channel 1: the simulated air needs every node on one channel
	// Low TX power: the phone is a meter away, and less 2.4 GHz energy inside the radio is better for its receiver
	WiFi.setTxPower(WIFI_POWER_8_5dBm);
	web.on("/", [] { web.send_P(200, "text/html", PAGE); });
	web.on("/api/state", HTTP_GET, api_state);
	web.on("/api/send", HTTP_POST, api_send);
	web.on("/api/call", HTTP_POST, api_call);
	web.begin();
	Serial.printf("WiFi AP %s, http://%s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
}

static void wifi_stop()
{
	web.stop();
	WiFi.softAPdisconnect(true);
	WiFi.mode(WIFI_OFF);
}

// ---- USB console --------------------------------------------------------------------------------------------------
static void console_line(char *line)
{
	char *cmd = strtok(line, " ");
	if (!cmd) {
		return;
	}
	if (!strcmp(cmd, "call")) {
		char *c = strtok(nullptr, " ");
		if (c && valid_call(c)) {
			set_call(c);
		}
		Serial.printf("call %s\n", own_call);
	} else if (!strcmp(cmd, "send")) {
		char *to = strtok(nullptr, " ");
		char *text = strtok(nullptr, "");
		if (to && text && send_text(to, text, nullptr)) {
			Serial.println("queued");
		} else {
			Serial.println("usage: send <TO|ALL> <text>");
		}
	} else if (!strcmp(cmd, "status")) {
		Serial.printf("call %s, radio %s (%s), queue %u, tx failures %lu, link errors %lu, relayed %u, "
			"suppressed %u, duplicates %u\n", own_call, radio_attached ? "attached" : "absent", radio_call,
			txq_count, (unsigned long)tx_failures, (unsigned long)link_errors, mesh.relayed, mesh.suppressed,
			mesh.duplicates);
#if GGM_SIM_RADIO
	} else if (!strcmp(cmd, "deaf") || !strcmp(cmd, "hear")) {
		char *c = strtok(nullptr, " ");
		if (c) {
			for (char *q = c; *q; q++) {
				*q = (char)toupper((unsigned char)*q);
			}
			sim_set_deaf(c, cmd[0] == 'd');
		}
		Serial.print("deaf to:");
		for (int i = 0; i < SIM_DEAF_MAX; i++) {
			if (sim_deaf[i][0]) {
				Serial.printf(" %s", sim_deaf[i]);
			}
		}
		Serial.println();
#endif
	} else if (!strcmp(cmd, "wifi")) {
		char *a = strtok(nullptr, " ");
		if (GGM_SIM_RADIO) {
			Serial.println("the simulated air needs WiFi: always on");
			return;
		}
		if (a) {
			bool on = !strcmp(a, "on");
			if (on != wifi_on) {
				wifi_on = on;
				prefs.putBool("wifi", on);
				on ? wifi_start() : wifi_stop();
			}
		}
		Serial.printf("wifi %s\n", wifi_on ? "on" : "off");
	} else {
		Serial.println(GGM_SIM_RADIO ? "commands: call <CALL> | send <TO|ALL> <text> | status | deaf <CALL> | hear <CALL>"
					     : "commands: call <CALL> | send <TO|ALL> <text> | status | wifi on|off");
	}
}

static void console_poll()
{
	static char buf[96];
	static size_t n;
	while (Serial.available()) {
		char c = (char)Serial.read();
		if (c == '\r' || c == '\n') {
			buf[n] = 0;
			if (n) {
				console_line(buf);
			}
			n = 0;
		} else if (n < sizeof buf - 1) {
			buf[n++] = c;
		}
	}
}

void setup()
{
	Serial.begin(115200);
	load_settings();
	radio.begin(GGM_LINK_BAUD, SERIAL_8N1, GGM_LINK_RX, GGM_LINK_TX);
	msg_kiss_rx_init(&krx);
	mesh_setup();
	if (GGM_SIM_RADIO) {
		wifi_on = true;
	}
	if (wifi_on) {
		wifi_start();
	}
#if GGM_SIM_RADIO
	sim_start();
	Serial.println("SIMULATED RADIO: frames go over ESP-NOW, no RF");
#endif
	Serial.printf("GGM mesh host, call %s, radio UART %d baud (RX %d, TX %d)\n", own_call, GGM_LINK_BAUD,
		GGM_LINK_RX, GGM_LINK_TX);
}

void loop()
{
	uint32_t now = millis();
	link_poll(now);
	msg_mesh_tick(&mesh, now);
	txq_pump(now);
	console_poll();
	if (wifi_on) {
		web.handleClient();
	}
	delay(2);
}

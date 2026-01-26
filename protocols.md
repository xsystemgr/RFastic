# FM-Mesh Protocol Specification

Version: 0.1  
Status: Experimental (core stable)  
License: MIT  

---

## 1. Εισαγωγή

Το FM-Mesh είναι ένα **custom mesh πρωτόκολλο ανταλλαγής μηνυμάτων πάνω από αναλογικούς FM πομποδέκτες**,
σχεδιασμένο για ESP32 nodes και απλά walkie-talkies (Quansheng, Baofeng κ.λπ.).

Το πρωτόκολλο **ΔΕΝ είναι συμβατό** με APRS, AX.25, LoRa, FT8, FT4 ή οποιοδήποτε υπάρχον σύστημα.
Έχει σχεδιαστεί αποκλειστικά για:

- real-time επικοινωνία
- mesh store & forward
- offline λειτουργία
- χαμηλή πολυπλοκότητα
- λειτουργία σε half-duplex FM

---

## 2. Στοίβα Πρωτοκόλλου (Protocol Stack)

Application (PC / Web / BLE / API)
└── Mesh Layer (routing, ACK, retry)
    └── Frame Layer (sync, length, CRC)
        └── Modem Layer (2-FSK baseband)
            └── Analog FM RF channel

---

## 3. Φυσικό Επίπεδο (Modem Layer)

### 3.1 Modulation
- Τύπος: 2-FSK baseband
- Bitrate: 600 bps (default)
- Σύμβολο: 1 bit
- TX: GPIO → MIC
- RX: SPK → ADC
- PTT: GPIO μέσω optocoupler

### 3.2 Ηλεκτρική Διασύνδεση
- TX: ψηφιακό επίπεδο με RC smoothing
- RX: audio σήμα με bias στο mid ADC
- Half-duplex λειτουργία

---

## 4. Framing (Frame Layer)

Κάθε μετάδοση έχει το ακόλουθο framing:

0x7E | LEN | PAYLOAD | CRC16

CRC: CCITT-16 (poly 0x1021, init 0xFFFF)

---

## 5. Mesh Payload (Mesh Layer)

TYPE | SRC | DST | MSGID | TTL | DATA

TYPE:
- 0x01 Broadcast
- 0x02 Direct (encrypted)
- 0x03 ACK
- 0x04 HELLO

---

## 6. Κρυπτογράφηση (μόνο Direct)

- AES-128
- CTR mode (stream)
- End-to-end
- Broadcast ποτέ κρυπτογραφημένο

Nonce = SRC | DST | MSGID | padding

---

## 7. ACK & Retry

- ACK frame: TYPE=0x03, DATA=MSGID
- Timeout: 1500 ms
- Retries: 3
- Random backoff 100–400 ms
- Drop μετά από αποτυχία

---

## 8. Mesh Forwarding

- Seen table (SRC+MSGID)
- TTL decrement
- Broadcast πάντα forward
- Direct forward configurable
- ACK ποτέ forward

---

## 9. HELLO / Discovery

TYPE=0x04  
DATA="HELLO <node_id>"

---

## 10. Transports

- WiFi SoftAP (REST / WS)
- BLE (planned)
- USB Serial (debug)

Όλα τα transports καταλήγουν στο ίδιο Mesh Layer.

---

## 11. Design Goals

- Απλότητα
- Χαμηλό latency
- Καμία εξάρτηση από internet
- Συμβατότητα με φθηνά FM HT
- Hackable firmware

---

## 12. Disclaimer

Πειραματικό πρωτόκολλο για εκπαίδευση, έρευνα και ham radio πειράματα.

# FM-Mesh-ESP32

Σύστημα **mesh επικοινωνίας μηνυμάτων πάνω από αναλογικούς FM πομποδέκτες**,
βασισμένο σε ESP32 (Arduino) και εμπνευσμένο από το Meshtastic.

Το project υλοποιεί **πλήρως custom πρωτόκολλο** (όχι APRS, όχι AX.25, όχι LoRa)
και επιτρέπει **offline επικοινωνία, chat και mesh forwarding** χωρίς internet.

---

## ✨ Χαρακτηριστικά

- 📡 Mesh networking (store & forward)
- 🔗 Custom 2-FSK modem πάνω από FM
- 🗣️ Συμβατό με οποιοδήποτε FM walkie-talkie
- 📨 Broadcast μηνύματα (χωρίς encryption)
- 🔐 Direct μηνύματα (AES encryption)
- 🔁 ACK + retry μηχανισμός
- 🧠 Dedupe (seen table)
- 🌐 WiFi SoftAP API (ESP32 ↔ PC χωρίς router)
- ⚙️ Arduino-first firmware
- 📱 BLE-ready (μελλοντικά)

---

## ❌ Τι ΔΕΝ είναι

- Δεν είναι APRS
- Δεν είναι AX.25
- Δεν είναι LoRa
- Δεν είναι FT8 / FT4
- Δεν απαιτεί internet

---

## 🧱 Αρχιτεκτονική

[ PC / Web / BLE App ]
│
▼
[ WiFi SoftAP API ]
│
▼
[ Mesh Layer ]
│
▼
[ 2-FSK Modem ]
│
▼
[ Αναλογικός FM Πομποδέκτης ]


---

## 🔧 Hardware

- ESP32-C3 Mini (Waveshare δοκιμασμένο)
- Quansheng UV-K1 / UV-K5 (K-plug)
- Optocoupler για PTT
- 2 αντιστάσεις + 1 πυκνωτής για audio

Δες τον φάκελο **hardware/** για σχέδια.

---

## 🌐 WiFi API

SSID: `FM-MESH`  
PASS: `mesh1234`  
IP: `192.168.4.1`

### Endpoints

GET /api/status
POST /api/send
GET /api/messages


> Όλα απαιτούν API key.

---

## 🚀 Γρήγορη Εκκίνηση

1. Άνοιξε `firmware/fm_mesh_esp32/` στο Arduino IDE  
2. Επίλεξε board: **ESP32C3 Dev Module**  
3. Ρύθμισε το `config.h`  
4. Upload στο ESP32  
5. Συνδέσου στο WiFi  
6. Στείλε μήνυμα 🎉

---

## ⚖️ Άδεια

MIT License © 2026 xsystemgr

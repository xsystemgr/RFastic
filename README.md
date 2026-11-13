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

---

## 🔌 Λειτουργία UART (ESP32 μέσα στον ασύρματο)

Δεύτερος τρόπος λειτουργίας, χωρίς audio modem: ο ασύρματος (BK4819) κάνει το FSK στο hardware και το ESP32
μιλάει με το MCU του μέσω UART. Στον αέρα παίζει το GGM2 του GOGUFW, άρα υπάρχει συμβατότητα με UV-K1/K5 που τρέχουν
GOGUFW. Το mesh (dedup, relay, ACK, retries) τρέχει στο ESP32.

- Πρωτόκολλο: [docs/bridge-protocol.md](docs/bridge-protocol.md) (KISS port 1)
- Portable C core + host tests: [`messenger/`](messenger/), περιγραφή στο [docs/gogu-messenger-core.md](docs/gogu-messenger-core.md)
- Firmware ESP32 (Seeed XIAO ESP32-C3, PlatformIO): [`esp32/ggm_bridge/`](esp32/ggm_bridge/)
- RT-880 (RadioFoundry_QXF): [`patches/radiofoundry-rt880-messenger-link.patch`](patches/radiofoundry-rt880-messenger-link.patch)

Κατάσταση: host tests περνάνε, το ESP32 firmware κάνει compile, το RT-880 image κάνει build. Δεν έχει δοκιμαστεί σε
hardware· ο UART driver του RT-880 είναι ακόμα stub.

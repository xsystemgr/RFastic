# FM-Mesh-ESP32

Σύστημα **mesh επικοινωνίας μηνυμάτων πάνω από αναλογικούς FM πομποδέκτες**,\nβασισμένο σε ESP32 (Arduino) και εμπνευσμένο από το Meshtastic.

Το project υλοποιεί **πλήρως custom πρωτόκολλο** (όχι APRS, όχι AX.25, όχι LoRa)\nκαι επιτρέπει **offline επικοινωνία, chat, και mesh forwarding** χωρίς internet.

---

## ✨ Χαρακτηριστικά

- 📡 Mesh networking (store & forward)\n- 🔗 Custom 2-FSK modem πάνω από FM\n- 🗣️ Συμβατό με οποιοδήποτε FM walkie-talkie\n- 📨 Broadcast μηνύματα (χωρίς encryption)\n- 🔐 Direct μηνύματα (AES encryption)\n- 🔁 ACK + retry μηχανισμός\n- 🧠 Dedupe (seen table)\n- 🌐 WiFi SoftAP API (ESP32 ↔ PC χωρίς router)\n- ⚙️ Arduino-first firmware\n- 📱 BLE-ready (μελλοντικά)

---

## ❌ Τι ΔΕΝ είναι

- Δεν είναι APRS\n- Δεν είναι AX.25\n- Δεν είναι LoRa\n- Δεν είναι FT8 / FT4\n- Δεν απαιτεί internet

---

## 🧱 Αρχιτεκτονική

(Περιγραφή αρχιτεκτονικής — πρόσθεσε εδώ διάγραμμα/περίληψη για nodes, modem layer, routing/store-and-forward, API και αποθήκευση μηνυμάτων.)

[ PC / Web / BLE App ]\n│\n▼\n[ WiFi SoftAP API ]\n│\n▼\n[ Mesh Layer ]\n│\n▼\n[ 2-FSK Modem ]\n│\n▼\n[ Αναλογικός FM Πομποδέκτης ]


---

## 🔧 Hardware

- ESP32-C3 Mini (Waveshare δοκιμασμένο)\n- Quansheng UV-K1 / UV-K5 (K-plug)\n- Optocoupler για PTT\n- 2 αντιστάσεις + 1 πυκνωτής για audio

Δες **hardware/** για σχέδια.

---

## 🌐 WiFi API

SSID: `FM-MESH`  \nPASS: `mesh1234`  \nIP: `192.168.4.1`

Endpoints:

GET /api/status\nPOST /api/send\nGET /api/messages\n+ Όλα απαιτούν API key.

+ ---

## 🚀 Γρήγορη Εκκίνηση

1. Άνοιξε `firmware/fm_mesh_esp32/` στο Arduino IDE\n2. Επίλεξε board: **ESP32C3 Dev Module**\n3. Ρύθμισε `config.h`\n4. Upload\n5. Συνδέσου στο WiFi\n6. Στείλε μήνυμα 🎉

---

## ⚖️ Άδεια

MIT License © 2026 xsystemgr
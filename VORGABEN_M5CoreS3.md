# Vorgaben: RFID-Controller mit M5CoreS3 + Modul-Stack

**Ziel:** Ersatz für den defekten M5StamPLC. Gleiche Funktionsidee (RFID, Relais, Strom/Spannung, Anzeige, Kommunikation zum Tab), andere Hardware.

---

## 1. Hardware-Übersicht

### Controller-Stack (von oben nach unten)

| Modul | Beschreibung |
|-------|--------------|
| **M5CoreS3** | Hauptrechner: ESP32-S3, 2" Touch-Display, WiFi, Bluetooth, USB-C, SD, Akku |
| **FAN V1.1** | Kühlung |
| **PWRCAN MODULE 13.2** | Power + CAN-Bus |
| **LAN Module 13.2** | Wired Ethernet |
| **4Relay 13.2 v1.1** | 4 Relais-Ausgänge |
| **PPS MODULE 13.2** | PPS/Timing (rotes Modul, Antenne) |
| **USB V1.2** | USB-Erweiterung |
| **CatM** (Basis) | SIM, NB-IoT, **RS-485**, **Wi-Fi** |

### Unverändert

- **M5Tab5** – HMI/Bedienung (WiFi 6 + Bluetooth 5.2 via ESP32-C6)
- **UHF-RFID** (z. B. U107) – bleibt, Anschluss per UART an CoreS3 oder passendem Modul

### INA226 (Strom/Spannung)

- **Option A:** INA226 **beibehalten** (I2C an CoreS3), falls kein Modul im Stack die gleiche Funktion hat.
- **Option B:** Prüfen, ob **PWRCAN** oder ein anderes Modul bereits Strom/Spannungsmessung bietet – dann INA226 weglassen.
- *Empfehlung:* Zunächst INA226 beibehalten; bei bestätigter PWRCAN-Messfunktion optional umstellen.

---

## 2. Kommunikation CoreS3 ↔ M5Tab5 (ohne eigenes WLAN)

**Vorgabe:** Es steht **kein eigenständiges WLAN** zur Verfügung. Die Kommunikation soll **direkt und drahtlos** zwischen CoreS3 und Tab5 laufen.

| Option | Beschreibung | Bewertung |
|--------|--------------|-----------|
| **ESP-NOW** | Direkt Gerät-zu-Gerät, kein Router/AP nötig. Unterstützt von ESP32-S3 (CoreS3) und ESP32-C6 (Tab5 WiFi-Chip). | **Umsetzung:** Siehe Projekt `cores3_rfid_controller` – Status wird per ESP-NOW gesendet (Broadcast oder an Tab5-MAC), Befehle vom Tab5 per ESP-NOW empfangen. |
| **Bluetooth (BLE)** | Direkt CoreS3 ↔ Tab5. | **Alternative**, falls ESP-NOW auf Tab5-Seite (C6) nicht genutzt werden soll. |
| **WiFi (nur falls später)** | CoreS3 als Access Point, Tab5 als Client – dann existiert „ein“ Netz nur zwischen den beiden. | Optional, wenn explizit IP-basiert gewünscht. |

**Aktueller Stand:** Im Projekt **cores3_rfid_controller** ist **ESP-NOW** umgesetzt: periodischer JSON-Status-Versand (Broadcast), Empfang von Befehlen (set_relay, rfid_learn, rfid_play). Tab5 muss ebenfalls ESP-NOW nutzen (C6), um zu senden/empfangen. CoreS3-MAC wird beim Start seriell ausgegeben, falls Tab5 gezielt an CoreS3 senden soll.

---

## 3. Funktionale Anforderungen (wie bisher angedacht)

- **RFID:** UHF-RFID lesen, Tags speichern/abgleichen, ggf. „rfid_learn“ / „rfid_play“ (Befehle vom Tab).
- **Relais:** 4 Relais über **4Relay**-Modul ansteuern (API des Moduls nutzen).
- **Strom/Spannung:** INA226 (oder Modul-Funktion) auslesen, Werte an Tab senden.
- **Anzeige:** CoreS3-Display für Status (RFID, U/I, Relais, Fehler).
- **Kommunikation:** Periodisch Status (JSON) an Tab5 senden; vom Tab5 Befehle (set_relay, rfid_learn, rfid_play, …) empfangen – Transport: WiFi (priorisiert), alternativ ESP-NOW oder BLE.

---

## 4. Projekt: cores3_rfid_controller

Im Ordner **cores3_rfid_controller/** liegt ein **PlatformIO-Projekt** mit:

- **Board:** M5CoreS3 (bzw. esp32-s3-devkitc-1 als Fallback)
- **Display:** M5Unified/M5.Display
- **I2C (Port A, G1/G2):** INA226 (0x41), 4Relay (0x26, Register 0x11)
- **UART (Port C, G17/G18):** UHF-RFID
- **Kommunikation:** **ESP-NOW** (kein WLAN nötig) – Status-Broadcast, Empfang von Befehlen (set_relay, rfid_learn, rfid_play)

**Build/Upload:** Im Ordner `cores3_rfid_controller` ausführen:
- `pio run -e cores3`
- `pio run -e cores3 -t upload`

**Tab5-Seite:** ESP-NOW auf dem Tab5 (ESP32-C6) muss noch implementiert werden: Empfang der Status-JSON, Sendung der Befehle (optional an CoreS3-MAC, die beim Start im Serial Monitor steht).

## 5. Nächste Schritte (technisch)

1. ~~**Neues Projekt** anlegen~~ → erledigt: **cores3_rfid_controller**
2. **4Relay:** Aktuell I2C 0x26, Reg 0x11 (Bits 0–3). Falls Modul 13.2 anderes Protokoll hat, anpassen.
3. **UHF-RFID:** Anschluss an Port C (G17 TX, G18 RX); Logik übernommen.
4. **Tab5:** ESP-NOW-Empfänger/Sender für gleiches JSON-Protokoll implementieren.
5. **PWRCAN/LAN/CatM:** Bei Bedarf später einbinden.

---

## 6. Referenz: Altes Protokoll (RS485, weiter nutzbar über WiFi/TCP)

Bisheriges JSON-Format (weiterverwendbar für Status und Befehle):

- **Status (Controller → Tab):**  
  `{"u":12.5,"i":0.5,"rfid":"E200...","list":[],"err":false,"relays":[0,0,1,0],"beep":0}`
- **Befehle (Tab → Controller):**  
  `set_relay`, `rfid_learn`, `rfid_play` (siehe README im archivierten Projekt).

Diese Nachrichten können 1:1 über TCP-Sockets oder HTTP-POST gesendet werden, sobald die WiFi-Verbindung steht.

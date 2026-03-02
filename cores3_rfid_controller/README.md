# M5CoreS3 RFID-Controller (ESP-NOW)

Controller für den Modul-Stack mit **M5CoreS3**. Kommunikation zum **M5Tab5** per **ESP-NOW** – **ohne eigenes WLAN**, direkt drahtlos.

## Hardware

- **M5CoreS3** (Display, WiFi für ESP-NOW)
- **4Relay** (I2C 0x26) – am Stack unten (M-Bus/I2C)
- **INA226** (I2C 0x41) – siehe Anschluss unten
- **UHF-RFID** (UART) – siehe Anschluss unten

---

## Anschluss: RFID-Modul und INA226

**Anschluss am CatM-Modul (oben im Stack):** INA226 an **Port A** (I2C = G12/G11), RFID an **Port C** (UART). Im Code: I2C = G12/G11; RFID = **RX=G18, TX=G17** (CoreS3 Port C), **115200** Baud. **RFID-Debug:** Serial Monitor (115200) zeigt bei Empfang `RFID raw N B: XX XX ...` – wenn nie erscheint: Pins in `main.cpp` auf **RX=16, TX=17** umstellen (generischer M-Bus) oder Baud auf **9600**.

Am **M5CoreS3** direkt (ohne CatM) gibt es seitlich zwei **HY2.0-4P**-Buchsen (Grove-kompatibel):

### INA226 (Strom/Spannung, I2C)

| Anschluss | CoreS3 **Port A** (rot, I2C) | Draht/Farbe (typ. Grove) |
|-----------|-----------------------------|---------------------------|
| **SDA**   | G2 (GPIO 21)       | Gelb |
| **SCL**   | G1 (GPIO 22)       | Weiß |
| **VCC**   | 5V (rot)           | Rot |
| **GND**   | GND (schwarz)      | Schwarz |

- **Buchse:** **Port A** (roter Stecker, I2C). 5V ist korrekt für PN532 und M5Stack-Ökosystem.
- I2C-Adresse im Code: **0x41** (bei anderer Adresse in `main.cpp` anpassen).
- **Hinweis:** 4Relay hängt am **Stack** (M-Bus). INA226 kannst du per **Adapterkabel** an Port A des **CoreS3** (oben am Gerät) anschließen, wenn du ein separates INA226-Modul nutzt.

### UHF-RFID (UART, z. B. U107)

| Anschluss | CoreS3 **Port C** (blau, UART) | Anschluss RFID-Modul |
|-----------|--------------------------------|----------------------|
| **CoreS3 RX** (G18) | Empfängt Daten | **TX** des RFID-Moduls (Daten **vom** Reader) |
| **CoreS3 TX** (G17) | Sendet Daten | **RX** des RFID-Moduls (falls der Reader Befehle entgegennimmt) |
| **GND** | GND | GND |
| **5V** | 5V (rot) | VCC (falls 5 V) |

- **Buchse:** **Port C** (blauer Stecker, UART).
- **Wichtig:** **TX des RFID-Moduls** → **RX des CoreS3**; **RX des RFID-Moduls** → **TX des CoreS3** (gekreuzt). Aktuell im Code: CatM Port C **RX=G17, TX=G16** (bei Bedarf in `main.cpp` tauschen).
- Baudrate: **9600** (`RFID_BAUD` in `main.cpp`; bei Leser 115200 anpassen).

### Kurz

- **INA226** → **Port A** (rot, I2C: SDA=G2, SCL=G1, 5V, GND).
- **RFID** → **Port C** (blau, UART: Reader-TX → G18, Reader-RX → G17, GND, 5V).

## Kommunikation

- **ESP-NOW:** Status-JSON wird periodisch gesendet (Broadcast); Befehle vom Tab5 werden per ESP-NOW empfangen.
- Kein Router/Access Point nötig.

## Build & Upload

```bash
cd cores3_rfid_controller
pio run -e cores3
pio run -e cores3 -t upload
```

Serial Monitor (z. B. für CoreS3-MAC und Logs):

```bash
pio device monitor -b 115200
```

**Upload-Port:** Wenn der CoreS3 angeschlossen ist, erscheint er z. B. als `/dev/ttyACM0` („M5Stack CoreS3“). Port prüfen mit `pio device list`. Bei Bedarf in `platformio.ini` eintragen: `upload_port = /dev/ttyACM0`. Zum Flashen ggf. **RESET** am CoreS3 **3 s** gedrückt halten (Download-Modus), dann Upload starten.

## Tab5

Auf dem M5Tab5 muss ESP-NOW (auf dem ESP32-C6) implementiert werden: Empfang der Status-Nachrichten, Senden der Befehle `set_relay`, `rfid_learn`, `rfid_play` (gleiches JSON-Format wie in den Vorgaben).

## Vorgaben

Siehe **[../VORGABEN_M5CoreS3.md](../VORGABEN_M5CoreS3.md)** im übergeordneten Projekt.

# M5CoreS3 RFID-Controller

Controller für den Modul-Stack mit **M5CoreS3**. Aktuelle Kommunikation zum **M5Tab5** per **WiFi SoftAP + UDP** (frühere ESP-NOW-Variante ist ersetzt).

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

- **WiFi SoftAP:** CoreS3 startet einen Access Point `CoreS3-AP` (Passwort `cores3pass`), Tab5 verbindet sich als Client.
- **UDP-Status (CoreS3 → Tab5):** Der CoreS3 sendet periodisch (alle 500 ms) ein Status-JSON per UDP-Broadcast an Port **4211**:

  ```json
  {"u":12.5,"i":0.5,"rfid":"E200...","list":["..."],"err":false,
   "wrerr":0,"wrok":1,"relays":[0,0,1,0],"beep":0,
   "pn532":1,"rc522":0,"charx_rs485":1,"emul":0}
  ```

- **UDP-Befehle (Tab5 → CoreS3):** Das Tab5 sendet per UDP-Broadcast an Port **4210**:

  - Relais schalten: `{"cmd":"set_relay","idx":0,"val":1}`
  - Scan starten/stoppen: `{"cmd":"rfid_scan_start"}`, `{"cmd":"rfid_scan_stop"}`
  - Tag lernen: `{"cmd":"rfid_learn"}`
  - Tag aus Liste löschen: `{"cmd":"rfid_delete","uid":"..."}`
  - Tag an CHARX / PN532 ausgeben:
    - RS-485 direkt: `{"cmd":"write_tag","uid":"...", "target":"rs485"}`
    - PN532-Emulation: `{"cmd":"write_tag","uid":"...", "target":"pn532"}` oder `{"cmd":"rfid_emulate","uid":"..."}`

  Die PN532-Emulation läuft pro Befehl ca. **3 Sekunden** (wie eine reale Karte kurz an den Reader halten) und initialisiert den PN532 dabei maximal dreimal. Danach wird der Target-Modus sauber beendet, um I2C-Hänger zu vermeiden.

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

Auf dem **M5Tab5** läuft ein passendes UI-Projekt (`tab5_controller`):

- Verbindet sich automatisch mit `CoreS3-AP`.
- Zeigt Status (Spannung/Strom, Relais, gelernte Tags, PN532-/RS-485-Fähigkeit).
- Sendet obige UDP-Befehle für **SCAN**, **Lernen/Löschen**, **Relais** und **Senden**.
- Ein globaler Schalter wählt den Kommunikationsweg **PN532** oder **RS-485** (CHARX 3150).  
  Der Button **„Senden“** pro Tag löst genau **eine** UID-Ausgabe über den gewählten Weg aus (pro Klick eine Emulation / ein RS-485-Telegramm).

### Tag-Bibliothek (SD-Karte) & Verhalten beim Start

- Gelernt werden die Tags über das **RFID2-Modul** (I2C 0x28) im Scan-Modus.
- Jeder per `rfid_learn` übernommene Tag landet
  - in einer **Arbeitsliste** (`rfidTagList`, max. 20 Einträge, die im Tab5-Hauptbildschirm angezeigt wird) und
  - zusätzlich in einer **dauerhaften Tag-Bibliothek** auf der SD-Karte (`/tags_lib.json`, max. 64 Einträge).
- Beim Neustart:
  - Die Arbeitsliste wird **nicht** automatisch befüllt – der Tab5-Hauptbildschirm startet leer.
  - Die Tag-Bibliothek wird aus der SD-Karte geladen; das Tab5 zeigt sie im Menü **„TAG-SPEICHER“** an.
  - Aus der Bibliothek können Tags gezielt in die Arbeitsliste übernommen („LADEN“) oder daraus gelöscht werden.
- Der CoreS3 sendet die komplette Bibliothek zusätzlich in jedem Status-Frame im Feld `taglib`, damit das Tab5 die Bibliothek auch bei Paketverlusten konsistent anzeigen kann.

Das TAG-SPEICHER-Overlay auf dem Tab5 ist als **modales Overlay** implementiert:  
Solange es geöffnet ist, wird nur das Overlay gezeichnet, wodurch kein „Hin-und-Her-Springen“ zwischen Haupansicht und Bibliothek mehr auftreten kann.

### Serial-Logging (Debug)

Im Projekt liegen zwei kleine Python-Skripte für strukturierte Logs:

- `cores3_rfid_controller/serial_debug.py`
  - Liest den **CoreS3** (typisch `/dev/ttyACM1`) mit 115200 Baud.
  - Schreibt eine NDJSON-Logdatei nach `.cursor/debug-405cb5.log`.
- `tab5_controller/tab5_serial_debug.py`
  - Liest das **M5Tab5** (typisch `/dev/ttyACM0`) mit 115200 Baud.
  - Schreibt eine NDJSON-Logdatei nach `.cursor/debug-tab5-405cb5.log`.

Damit können S3- und Tab5-Verhalten (I2C-Scan, PN532, UDP-Kommandos, Tag-Bibliothek, Abstürze) zeitlich sauber korreliert werden.

## Vorgaben

Siehe **[../VORGABEN_M5CoreS3.md](../VORGABEN_M5CoreS3.md)** im übergeordneten Projekt.

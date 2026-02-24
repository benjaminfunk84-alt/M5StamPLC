# Debug-Session Protokoll – M5CoreS3 RFID Controller

Cursor-Transkript-ID: `405cb58d-7afa-4b6c-bc98-c07c5a713036`

---

## Session vom 24.02.2026 – Zusammenfassung

### Bestätigte Fixes (laufzeitverifiziert)

#### 1. INA226 – Falscher Formel-Shift (BEHOBEN)
- **Bug:** `float vBus = (busRaw >> 3) * 1.25e-3f;` — der `>> 3` Shift ist für den INA219, nicht den INA226
- **Fix:** `float vBus = busRaw * 1.25e-3f;`
- **Beweis:** `busRaw=9777` → vorher `1.53V`, jetzt korrekt `12.22V`
- **Datei:** `cores3_rfid_controller/src/main.cpp`, Funktion `readINA226At()`

#### 2. RFID – Falsche UART-Pins und Baud-Rate (BEHOBEN)
- **Bug:** Pins waren auf GPIO13(RX)/GPIO14(TX) @ 9600Bd — lieferten 0 Bytes
- **Diagnose:** UART-Scanner testete 12 Pin/Baud-Kombos automatisch
- **Ergebnis:** Nur `RX=18, TX=17 @ 115200Bd` antwortete (8 Bytes = YRM100-Fehler-Frame = kein Tag)
- **Fix:** `#define RFID_RX_PIN 18`, `#define RFID_TX_PIN 17`, `#define RFID_BAUD 115200`
- **Bedeutung:** GPIO18/GPIO17 ist der Standard M5Stack CoreS3 Port C

#### 3. RFID – Protokoll auf YRM100-Binärformat umgestellt (BEHOBEN)
- **Bug:** Firmware las ASCII-Zeilen (`readLineFromSerial`) — YRM100 nutzt Binärprotokoll
- **Fix:** `pollYRM100()` Funktion implementiert
  - Poll-Befehl: `BB 00 22 00 00 22 7E`
  - Wird alle 300ms gesendet
  - Erfolgs-Frame: `BB 01 22 00 11 [RSSI][PC×2][EPC×12][CRC×2][checksum] 7E` (24 Bytes)
  - Fehler-Frame (kein Tag): `BB 01 FF 00 01 [err][checksum] 7E` (8 Bytes)
  - EPC wird aus Bytes [8..19] (12 Bytes = 96-bit) als Hex-String extrahiert

#### 4. I2C – Wire1 für externe Sensoren (BEHOBEN, frühere Session)
- **Fix:** INA226 und 4Relay laufen auf `Wire1` (SDA=21, SCL=22 = Port A)
- **Grund:** Kein Bus-Konflikt mit M5Unified (nutzt `Wire` intern für Display/Touch)

---

## Offene Punkte (nächste Session)

### RFID Tag-Erkennung – Frame-Parsing unbestätigt

**Status:** RFID-Polling läuft korrekt, aber ob Tags korrekt erkannt werden ist noch zu prüfen.

**Symptom:** Obwohl ein Tag auf dem Leser lag, erschien kein `RFID Tag: [EPC]` in der Ausgabe.

**Debug-Instrumentation aktiv:**
```
DBG POLL got=X: BB 01 ...   ← zeigt rohe Antwort-Bytes (alle 2s)
DBG H9: RFID poll – kein Tag ← zeigt wenn kein Tag erkannt (alle 5s)
```

**Mögliche Ursachen:**
1. `pollYRM100()` wartet 250ms auf >= 8 Bytes — könnte zu wenig sein wenn Tag-Response länger dauert
2. Frame-Offset stimmt nicht (EPC bei byte[8] angenommen) — muss mit `DBG POLL` Bytes verifiziert werden
3. YRM100 könnte beim M5Stack Modul leicht abweichendes Frame-Format haben

**Nächster Schritt:**
1. `python3 cores3_rfid_controller/serial_debug.py` starten
2. RFID-Tag direkt auf Leser legen
3. Im Output nach `DBG POLL got=24: BB 01 22 ...` suchen
4. Bytes ab Position 8 sind der EPC — mit erwartetem Tag-Wert vergleichen

---

## Hardware-Konfiguration (bestätigt)

| Komponente | Interface | Pins | Baud/Speed |
|------------|-----------|------|------------|
| INA226 | I2C (Wire1) | SDA=21, SCL=22 (Port A) | 100kHz |
| 4Relay | I2C (Wire1) | SDA=21, SCL=22 (Port A) | 100kHz |
| UHF-RFID (YRM100) | UART1 | RX=18, TX=17 (Port C) | 115200 |
| Display/Touch | I2C (Wire intern) | G12/G11 | via M5Unified |

**INA226 Adresse:** 0x41 (bestätigt)
**4Relay Adresse:** 0x26

**Module-Stack:** CoreS3 → FAN → PWRCAN → LAN → 4Relay → PPS → USB → CatM

---

## Wichtige Dateien

| Datei | Beschreibung |
|-------|-------------|
| `cores3_rfid_controller/src/main.cpp` | Haupt-Firmware mit Debug-Instrumentation |
| `cores3_rfid_controller/platformio.ini` | PlatformIO-Konfiguration (upload_port=/dev/ttyACM0) |
| `cores3_rfid_controller/serial_debug.py` | Serial-Capture-Skript → schreibt NDJSON in `.cursor/debug-405cb5.log` |
| `cores3_rfid_controller/i2c_scan/` | Standalone I2C-Scanner Utility |

## Flash-Befehl

```bash
cd cores3_rfid_controller
pio run -e cores3 -t upload
```

## Serial-Monitor

```bash
python3 cores3_rfid_controller/serial_debug.py
# oder direkt:
pio device monitor -b 115200
```

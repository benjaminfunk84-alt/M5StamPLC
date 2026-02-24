# Debugging – M5StampPLC

> **Hinweis:** Dieses Projekt ist archiviert (StamPLC defekt). Neues Ziel: **M5CoreS3** – siehe [VORGABEN_M5CoreS3.md](VORGABEN_M5CoreS3.md).

## Häufige Probleme und Lösungen

### 1. Display bleibt schwarz
- **Ursache:** Falsche oder fehlende Board-Initialisierung.
- **Lösung:** Mit diesem Projekt werden **M5StamPLC**, **M5Unified** und **M5GFX** über `platformio.ini` geladen. Beim Start wird `M5StamPLC.begin()` aufgerufen – das initialisiert Display und Backlight. Seriell sollte „HW: M5StamPLC“ erscheinen.
- **Prüfen:** Serial Monitor (115200 Baud): Zeile „HW: M5StamPLC (Relays/INA226/RS485 via API)“?

### 2. Relais reagieren nicht
- **Ursache:** Auf dem echten StamPLC steuert der **AW9523** die Relais, nicht direkte GPIOs.
- **Lösung:** Im Code wird bei M5StamPLC `M5StamPLC.writePlcRelay(channel, state)` verwendet. Kein `digitalWrite` auf GPIO 12–15.
- **Prüfen:** Serial: „Relays initialized (all OFF, via M5StamPLC)“.

### 3. RS485 keine Kommunikation
- **Ursache:** StamPLC nutzt andere Pins als ein generischer ESP32.
- **Lösung:** Beim StamPLC sind RS485-Pins **TX=0, RX=39, DIR=46** (PWR-485-Port). Der Code wechselt automatisch auf diese Pins, wenn `M5StamPLC.h` gefunden wird.
- **Prüfen:** Richtige Adern am PWR-485 (A/B bzw. D+/D−); Baudrate 115200.

### 4. INA226 (Spannung/Strom) immer 0
- **Ursache:** StamPLC nutzt internen I2C (SDA=13, SCL=15); eigenes `Wire.begin(21,22)` würde stören.
- **Lösung:** Mit M5StamPLC werden **keine** eigenen I2C-Pins gesetzt. Es werden `M5StamPLC.getPowerVoltage()` und `M5StamPLC.getIoSocketOutputCurrent()` verwendet.
- **Prüfen:** Display/Serial: „U:“ und „I:“ – bei fehlendem Sensor oder falscher Verdrahtung bleiben Werte 0.

### 5. Serial Monitor verbindet nicht / Write timeout beim Upload
- **Ursache:** USB-CDC (USB-Serial/JTAG) oder falscher Port.
- **Lösung:**
  - Nur **ein** Programm nutzt den Port (Monitor schließen vor Upload).
  - Beim Upload: **Boot-Taste** gedrückt halten, bis „Connecting…“ erscheint, dann loslassen.
  - Port prüfen: `pio device list` (oft `/dev/ttyACM0`).
- **Optional:** In `platformio.ini` fest setzen: `upload_port = /dev/ttyACM0`.

### 6. Build: „DISPLAY redefined“
- **Ursache:** `Arduino.h` definiert ein Makro `DISPLAY`.
- **Lösung:** Im Projekt wird das Display-Makro **M5_DISPLAY** genannt, um Kollisionen zu vermeiden.

### 7. Debugger (Breakpoints)
- **PlatformIO:** Über die PlatformIO-Erweiterung „Debug“ starten (icon/Command Palette). ESP32-S3 unterstützt USB-JTAG (ohne externen Adapter), wenn in `platformio.ini` z. B. `debug_tool = esp-builtin` gesetzt ist.
- **Serial:** Für Log-Ausgaben `Serial.printf()` nutzen und mit **PlatformIO: Serial Monitor** (115200) beobachten.

---

## Nützliche Befehle (im Projektordner)

| Aktion        | Befehl                          |
|---------------|----------------------------------|
| Build         | `pio run`                        |
| Upload        | `pio run -t upload`              |
| Serial Monitor| `pio device monitor -b 115200`   |
| Clean         | `pio run -t clean`               |
| Geräte anzeigen | `pio device list`             |

---

## Zwei Modi im Code

- **Mit M5StamPLC-Bibliothek** (normale StamPLC-Hardware):  
  RS485-Pins 0/39/46, Relais und INA226 über M5StamPLC-API, I2C wird nicht manuell gestartet.

- **Ohne M5StamPLC** (z. B. anderes ESP32-Board):  
  RS485 17/16/4, Relais GPIO 12–15, INA226 per I2C (21/22, Adresse 0x41).

Die richtige Variante wird automatisch per `__has_include(<M5StamPLC.h>)` gewählt.

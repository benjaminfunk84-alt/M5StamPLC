# M5StamPLC – RFID-Controller

> **Archiv:** Dieses Projekt ist archiviert (M5StamPLC defekt). Die Vorgaben wurden auf **M5CoreS3 + Zusatzmodule** umgestellt. Siehe **[ARCHIVE.md](ARCHIVE.md)** und **[VORGABEN_M5CoreS3.md](VORGABEN_M5CoreS3.md)**.

---

Arduino-Sketch für **M5StampPLC** mit UHF-RFID (U107), INA226 (Strom/Spannung), 4× Relais und RS485-Kommunikation zum M5Tab.

## Hardware

- **M5StampPLC** (ESP32)
- **UHF-RFID** Modul U107 (UART1)
- **INA226** Strom-/Spannungssensor (I2C, Adresse 0x41)
- **4× Relais** (GPIO 12–15)
- **RS485** (UART2) für Kommunikation mit M5Tab

## Abhängigkeiten

- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- Arduino ESP32 Core (Wire, EEPROM)
- **Für das kleine Display:** [M5StamPLC](https://github.com/m5stack/M5StamPLC) (empfohlen, zieht M5Unified/M5GFX nach) oder nur [M5Unified](https://github.com/m5stack/M5Unified). **M5StamPLC** initialisiert das Display hardwaregerecht.

## Pinbelegung

| Funktion    | GPIO |
|------------|------|
| RS485 TX   | 17   |
| RS485 RX   | 16   |
| RS485 DIR  | 4    |
| RFID RX    | 26   |
| RFID TX    | 25   |
| Relais 1–4 | 12–15|
| I2C SDA/SCL| 21/22|

## RS485-Protokoll

Der StamPLC sendet periodisch (alle 200 ms) ein JSON-Statuspaket an das M5Tab:

```json
{"u":12.5,"i":0.5,"rfid":"E200...","list":[],"err":false,"relays":[0,0,1,0],"beep":0}
```

Befehle vom M5Tab (JSON, zeilenweise):

- **Relais schalten:** `{"cmd":"set_relay","idx":0,"val":1}`
- **RFID lernen:** `{"cmd":"rfid_learn"}`
- **RFID abspielen:** `{"cmd":"rfid_play","tag":"E200..."}`

## Entwicklung in Cursor (ohne Arduino IDE)

Das Projekt ist als **PlatformIO**-Projekt eingerichtet. Alle Display-Bibliotheken (**M5StamPLC**, **M5Unified**, **M5GFX**) werden über `platformio.ini` geladen – kein manuelles Installieren nötig.

- **Quellcode:** `src/main.cpp` (entspricht dem bisherigen `.ino`).
- **Build:** Terminal `pio run` oder in Cursor **Tasks: „PlatformIO: Build“** (Strg+Shift+B).
- **Upload:** `pio run -t upload` oder Task **„PlatformIO: Upload“**. Vorher StamPLC per USB verbinden; ggf. **Boot-Taste** gedrückt halten beim Upload.
- **Serieller Monitor (Debug):** `pio device monitor -b 115200` oder Task **„PlatformIO: Serial Monitor“**.

Environment: `m5stamplc` (Board: M5Stack StampS3). Falls das Board nicht erkannt wird, in `platformio.ini` das Environment `m5stamplc_esp32s3` verwenden.

## Installation (Arduino IDE)

1. Arduino IDE öffnen.
2. Bibliotheken installieren (Library Manager): **ArduinoJson**, für das Display **M5StamPLC** (empfohlen) oder **M5Unified**.
3. Board wählen: **M5Stamp PLC** (unter „M5Stack Boards“ bzw. „Board-Verwaltung“). Wenn du nur „ESP32 Dev Module“ wählst, erkennt M5Unified das Display evtl. nicht.
4. Ordner `M5StamPLC_Arduino` als Sketch öffnen.
5. Kompilieren und auf den StamPLC flashen.

**Probleme beim Debuggen?** Siehe **[DEBUG.md](DEBUG.md)** für typische Fehler (Display schwarz, Relais/RS485/INA226, Serial/Upload) und Lösungen.

**Display bleibt schwarz?**  
- **M5StamPLC**-Bibliothek installieren (Library Manager → M5StamPLC) – initialisiert das Display korrekt
- Alternativ M5Unified nutzen (oft nur mit korrekter Board-Auswahl)
- Board **M5Stamp PLC** ausgewählt? (nicht nur „ESP32 Dev Module“)
- Seriellen Monitor öffnen (115200 Baud): Steht dort „Display: M5Unified aktiv“ oder „Display: aus“?

## Lizenz

MIT (oder deine gewählte Lizenz).

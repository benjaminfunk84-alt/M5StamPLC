# M5StamPLC – RFID-Controller

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

## Installation

1. Arduino IDE oder PlatformIO öffnen.
2. Bibliotheken installieren (Library Manager): **ArduinoJson**, für das Display **M5StamPLC** (empfohlen) oder **M5Unified**.
3. Board wählen: **M5Stamp PLC** (unter „M5Stack Boards“ bzw. „Board-Verwaltung“). Wenn du nur „ESP32 Dev Module“ wählst, erkennt M5Unified das Display evtl. nicht.
4. Ordner `M5StamPLC_Arduino` als Sketch öffnen oder als Projekt verwenden.
5. Kompilieren und auf den StamPLC flashen.

**Display bleibt schwarz?**  
- **M5StamPLC**-Bibliothek installieren (Library Manager → M5StamPLC) – initialisiert das Display korrekt
- Alternativ M5Unified nutzen (oft nur mit korrekter Board-Auswahl)
- Board **M5Stamp PLC** ausgewählt? (nicht nur „ESP32 Dev Module“)
- Seriellen Monitor öffnen (115200 Baud): Steht dort „Display: M5Unified aktiv“ oder „Display: aus“?

## Lizenz

MIT (oder deine gewählte Lizenz).

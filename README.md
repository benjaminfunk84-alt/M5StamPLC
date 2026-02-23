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
2. Bibliothek **ArduinoJson** installieren (Library Manager).
3. Ordner `M5StamPLC_Arduino` als Sketch öffnen oder als Projekt verwenden.
4. Board: **ESP32** (passendes M5StampPLC-Board auswählen).
5. Kompilieren und auf den StamPLC flashen.

## Lizenz

MIT (oder deine gewählte Lizenz).

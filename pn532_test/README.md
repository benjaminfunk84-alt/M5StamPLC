# PN532 Modul-Test

Eigenständiges Test-Projekt **nur** für das PN532-Modul am M5Stack CoreS3.  
Kein M5/Display, kein INA226/RFID2/4Relay – nur CoreS3 + PN532 an Port A (I2C).

## Verkabelung

- **CoreS3 Port A (Grove HY2.0-4P, rot):** SDA = **GPIO 21**, SCL = **GPIO 22**, 5V, GND.  
- **PN532:** VCC → 5V, GND → GND, SDA → 21, SCL → 22 (am Port A).  
- 5V VCC ist korrekt (PN532-Layout/Ökosystem); Port A liefert 5V.  
- DIP-Schalter am PN532-Board auf **I2C** stellen (nicht SPI/UART).

## Ablauf

1. Alles andere abziehen, **nur** PN532 an Port A stecken.
2. CoreS3 per USB verbinden.
3. Firmware flashen und Serial-Monitor öffnen:

```bash
cd pn532_test
pio run -e pn532 -t upload
pio device monitor -b 115200
```

4. Im Log prüfen:
   - **I2C-Scan:** Es muss mindestens `0x24` erscheinen.
   - **PN532 begin() / SAMConfig() / TgInitAsTarget:** Alle drei mit "OK".
   - Danach: Reader (CHARX oder NFC-Handy) an den PN532 halten → es erscheinen Zeilen `[Reader-Daten N Bytes]: ...`.

## Zurück zur Haupt-Firmware

Nach dem Test die normale RFID-Controller-Firmware wieder flashen:

```bash
cd ../cores3_rfid_controller
pio run -e cores3 -t upload
```

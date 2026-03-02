# I2C-Scan (nur Port A)

Scant **nur Port A** (Wire1, SDA=21, SCL=22) alle 5 Sekunden.

- **Port A (Wire1, 21/22)** – Grove Port A am CoreS3 (rot), 5V/GND – PN532, INA226, 4Relay

Der interne Bus (Wire 12/11) wird **nicht** gescannt: Jeder Aufruf von `Wire.begin(12,11)` macht auf dem CoreS3 danach Wire1 unbrauchbar („Invalid pin: 22“). Für Port A reicht dieser Scan.

## Ablauf

```bash
cd cores3_rfid_controller/i2c_scan
pio run -e cores3 -t upload
pio device monitor -b 115200
```

Falls das Gerät an anderem Port hängt (z. B. `/dev/ttyACM1`):

```bash
pio run -e cores3 -t upload --upload-port /dev/ttyACM1
pio device monitor -b 115200 -p /dev/ttyACM1
```

## Ausgabe

- Adressen 0x08–0x77 auf Port A, alle 5 s.
- Hinweise: 0x24 = PN532/RC522, 0x40/0x41 = INA226, 0x26 = 4Relay.

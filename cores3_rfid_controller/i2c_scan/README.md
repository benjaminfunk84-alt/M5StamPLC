# I2C-Scan (beide Busse)

Kleines Programm, das **beide** möglichen I2C-Busse scannt und alle gefundenen Adressen ausgibt:

- **Port A (G2/G1)** – M-Bus Pin 19/20, klassischer externer Port
- **Intern (G12/G11)** – M-Bus Pin 17/18, oft Anschluss für CatM Port A (Display/Kamera nutzen diesen Bus)

## Ablauf

```bash
cd cores3_rfid_controller/i2c_scan
pio run -t upload
pio device monitor -b 115200
```

Falls das Gerät an anderem Port hängt (z. B. `/dev/ttyACM1`):

```bash
pio run -t upload --upload-port /dev/ttyACM1
pio device monitor -b 115200 -p /dev/ttyACM1
```

## Ausgabe

- Alle Adressen von 0x08 bis 0x77 werden geprüft.
- Gefundene Adressen werden mit Hinweisen (z. B. 4Relay, INA226) ausgegeben.
- Der Scan läuft alle 5 Sekunden erneut.

Mit der Liste der Adressen kannst du im Hauptprojekt prüfen, ob INA226 (0x40/0x41) und 4Relay (0x26) am Bus hängen.

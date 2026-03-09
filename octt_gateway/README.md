## OCTT SUT Gateway – CoreS3 / CHARX 3150

Dieses kleine Node.js‑Projekt stellt die **OCTT SUT API CS** bereit und mapped die Aufrufe auf den bestehenden **CoreS3 RFID‑Controller** und die **CHARX 3150**.

### Architektur

- **Gateway (dieses Projekt)** läuft auf einem PC (Windows oder Linux).
- **CoreS3**:
  - IP: `192.168.0.33`
  - UDP‑Kommandos: Port `4210` (`{"cmd":"…", ...}` – wie vom Tab5).
  - UDP‑Status: Port `4211` (JSON‑Status alle 500 ms).
- **CHARX 3150**:
  - IP: `192.168.0.11`
  - Hängt über PwrCAN/RS485 am CoreS3.
- **Relais am CoreS3** (über 4Relay/Externe Relais, NC):
  - `relay[0]` – CHARX‑Versorgung trennen (Reboot).
  - `relay[1]` – Temperatursensor wegschalten (Fault).
  - `relay[2]` – CP‑Signal unterbrechen (Unplug).
  - `relay[3]` – Parkplatzsensor (occupied/unoccupied).

Der PC, der CoreS3 und die CHARX hängen an einem gemeinsamen Switch (Subnetz `192.168.0.0/24`).

### Installation

```bash
cd octt_gateway
npm install
node server.js
```

Standard‑Port ist `8080` (konfigurierbar in `config.json`).

### Konfiguration

Die Datei `config.json` enthält:

- `listenPort` – HTTP‑Port des Gateways (Standard: `8080`).
- `coreS3Host` – IP des CoreS3 (Standard: `192.168.0.33`).
- `coreS3CmdPort` – UDP‑Port für Befehle (`4210`).
- `coreS3StatusPort` – UDP‑Port für Status (`4211`).
- `charxHost` – IP der CHARX 3150 (derzeit nur informativ).
- `defaultEvseId`, `defaultConnectorId` – Default‑Parameter für OCTT.
- `relay.{charxPower,tempSensor,cpUnplug,parkingSensor}` – Zuordnung der Relais‑Indizes.

Konfiguration kann auch über die Web‑GUI geändert werden:

```text
http://<PC-IP>:8080/
```

### Web‑GUI

Unter `http://<PC-IP>:8080/` stehen bereit:

- **Konfiguration**: CoreS3‑IP/Ports, CHARX‑IP, Default‑EVSE/Connector.
- **SUT‑Testaufrufe**: Buttons für `/authorize`, `/plugin`, `/plugout`, `/parkingbay`, `/state`, `/reboot`.
- **Status**: Anzeige des letzten Status‑JSON vom CoreS3 (über UDP‑Port 4211).
- **Log**: einfache Textausgabe der zuletzt ausgeführten Aktionen.

### SUT‑API (für OCTT)

Basis‑URL (im OCTT als `base-url` zu setzen):

```text
http://<PC-IP>:8080/api/v1
```

Unterstützte Endpunkte:

- `POST /plugin`
  - Simuliert „Kabel eingesteckt“ auf der Charging‑Station‑Seite.
  - Aktion: Relais `cpUnplug` wird **abgeschaltet** → externes NC schließt CP‑Leitung.
- `POST /plugout`
  - Simuliert „Kabel ausgesteckt“.
  - Aktion: Relais `cpUnplug` wird **angezogen** → externes NC öffnet CP‑Leitung.
- `POST /parkingbay?occupied=true|false`
  - Setzt den Parkplatzsensor über Relais `parkingSensor`.
- `POST /authorize?id=UID[&type=&evse_id=&connector_id=]`
  - UID (RFID/ID‑Token) von OCTT.
  - Aktion: UDP zum CoreS3
    - `{"cmd":"write_tag","uid":"<id>","target":"rs485"}`
  - CoreS3 sendet UID als ASCII + `0x0D` per RS485 an CHARX 3150.
- `POST /reboot`
  - Reboot der Charging Station.
  - Aktion: Relais `charxPower` wird ~3 s angezogen (CHARX stromlos), dann wieder freigegeben.
- `POST /state?...`
  - Aktuell unterstützt:
    - `faulted=true|false` → Relais `tempSensor` schaltet einen Temperatursensor weg bzw. wieder zu.
  - Weitere Parameter (`unlock_failed`, `refused_local_auth_list`, `charging_limit`) werden derzeit nur geloggt.

Zusätzlich:

- `GET /health` – einfacher Health‑Check (u.a. Alter des letzten Status‑Frames vom CoreS3).
- `GET /ui/config`, `POST /ui/config` – Konfiguration lesen/schreiben.
- `GET /ui/status` – letzten Status vom CoreS3 (zur Diagnose).

### Nutzung mit OCTT

1. Gateway auf dem PC starten (`node server.js`).
2. Im Browser `http://<PC-IP>:8080/` öffnen und `CoreS3Host`/Ports prüfen.
3. In der **OCTT Swagger‑UI** (`SUT API CS`) `base-url` so setzen, dass die Requests beim Gateway ankommen, z.B.:

   ```text
   base-url = 192.168.0.10:8080
   → OCTT ruft http://192.168.0.10:8080/api/v1/... auf
   ```

4. Testfälle mit „Try it out“ ausführen – das Gateway übersetzt die Aufrufe auf
   CoreS3‑Kommandos und Relaisaktionen, die wiederum den CHARX 3150 beeinflussen.


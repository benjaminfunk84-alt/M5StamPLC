// OCTT SUT Gateway – PC-Seite
// Stellt die SUT API CS bereit und mapped sie auf CoreS3/CHARX-Funktionen.

const fs = require("fs");
const path = require("path");
const dgram = require("dgram");
const express = require("express");
const morgan = require("morgan");

const CONFIG_PATH = path.join(__dirname, "config.json");

function loadConfig() {
  try {
    const raw = fs.readFileSync(CONFIG_PATH, "utf8");
    return JSON.parse(raw);
  } catch (e) {
    console.error("Config konnte nicht gelesen werden, verwende Defaults:", e.message);
    return {
      listenPort: 8080,
      coreS3Host: "192.168.0.33",
      coreS3CmdPort: 4210,
      coreS3StatusPort: 4211,
      charxHost: "192.168.0.11",
      defaultEvseId: "1",
      defaultConnectorId: "1",
      relay: {
        charxPower: 0,
        tempSensor: 1,
        cpUnplug: 2,
        parkingSensor: 3
      }
    };
  }
}

function saveConfig(cfg) {
  fs.writeFileSync(CONFIG_PATH, JSON.stringify(cfg, null, 2));
}

const config = loadConfig();

// UDP-Sockets zum CoreS3
const udpCmd = dgram.createSocket("udp4");
const udpStatus = dgram.createSocket("udp4");

let lastStatus = null;

udpStatus.on("error", (err) => {
  console.error("udpStatus error:", err);
});

udpStatus.on("message", (msg, rinfo) => {
  try {
    const txt = msg.toString("utf8");
    const js = JSON.parse(txt);
    lastStatus = { data: js, from: rinfo, ts: Date.now() };
  } catch (e) {
    // Status kann auch andere Zeilen enthalten, ignoriere Parse-Fehler
  }
});

udpStatus.bind(config.coreS3StatusPort, () => {
  console.log(`UDP-Status lauscht auf 0.0.0.0:${config.coreS3StatusPort}`);
});

function sendToCoreS3(obj) {
  const buf = Buffer.from(JSON.stringify(obj));
  udpCmd.send(buf, config.coreS3CmdPort, config.coreS3Host, (err) => {
    if (err) {
      console.error("UDP-Command Fehler:", err);
    } else {
      console.log("UDP → CoreS3:", obj);
    }
  });
}

function setRelay(idx, on) {
  sendToCoreS3({ cmd: "set_relay", idx, val: on ? 1 : 0 });
}

// Express-App
const app = express();
app.use(express.json());
app.use(morgan("dev"));

// Einfache UI / Status-API
app.get("/ui/config", (req, res) => {
  res.json(config);
});

app.post("/ui/config", (req, res) => {
  Object.assign(config, req.body || {});
  saveConfig(config);
  res.json({ ok: true, config });
});

app.get("/ui/status", (req, res) => {
  if (!lastStatus) {
    return res.status(503).json({ ok: false, message: "Noch kein Status vom CoreS3 empfangen." });
  }
  res.json({ ok: true, lastStatus });
});

app.use("/", express.static(path.join(__dirname, "public")));

// Hilfsfunktionen zum Parsen von Bool/Strings aus Query
function parseBool(v) {
  if (v === undefined) return undefined;
  if (typeof v === "boolean") return v;
  const s = String(v).toLowerCase();
  return s === "1" || s === "true" || s === "yes";
}

// ===== SUT API CS – Endpunkte =====

// /plugin – Kabel auf der Charging-Station-Seite einstecken
app.post("/api/v1/plugin", (req, res) => {
  const halfway = parseBool(req.query.halfway);
  console.log("SUT /plugin", req.query);

  // CP-Signal über Relais simulieren:
  // - halfway=true könnte zukünftig speziellen Fehler erzeugen,
  //   für den Anfang unterscheiden wir nicht und sorgen nur dafür,
  //   dass CP "eingesteckt" ist (Relais aus = NC geschlossen).
  setRelay(config.relay.cpUnplug, false);

  res.sendStatus(200);
});

// /plugout – Kabel ausstecken (CP trennen)
app.post("/api/v1/plugout", (req, res) => {
  console.log("SUT /plugout", req.query);
  // CP-Leitung über Relais unterbrechen: Relais anziehen → externes NC öffnet
  setRelay(config.relay.cpUnplug, true);
  res.sendStatus(200);
});

// /parkingbay – Parkplatzsensor belegt/frei
app.post("/api/v1/parkingbay", (req, res) => {
  const occupied = parseBool(req.query.occupied);
  console.log("SUT /parkingbay", req.query, "→ occupied:", occupied);
  if (occupied === undefined) return res.status(400).send("missing occupied=true|false");
  setRelay(config.relay.parkingSensor, !!occupied);
  res.sendStatus(200);
});

// /authorize – ID-Token autorisieren (RFID/UID)
app.post("/api/v1/authorize", (req, res) => {
  const id = req.query.id;
  const type = req.query.type || "Central";
  const evseId = req.query.evse_id || config.defaultEvseId;
  const connectorId = req.query.connector_id || config.defaultConnectorId;

  if (!id) return res.status(400).send("missing id");

  console.log("SUT /authorize", { id, type, evseId, connectorId });

  // UID an CoreS3 weitergeben → RS485 an CHARX
  sendToCoreS3({ cmd: "write_tag", uid: id, target: "rs485" });

  res.sendStatus(200);
});

// /reboot – Charging Station rebooten (hier: CHARX über Relais kurz stromlos)
app.post("/api/v1/reboot", (req, res) => {
  console.log("SUT /reboot");

  // CHARX-Netzteil über Relais kurz trennen (Relais zieht an → externes NC öffnet)
  setRelay(config.relay.charxPower, true);
  setTimeout(() => {
    setRelay(config.relay.charxPower, false);
  }, 3000); // 3 s ausreichen, ggf. anpassen

  res.sendStatus(200);
});

// /state – diverse Zustände setzen (vereinfacht auf das, was wir per Relais können)
app.post("/api/v1/state", (req, res) => {
  console.log("SUT /state", req.query);

  const faulted = parseBool(req.query.faulted);
  const unlockFailed = parseBool(req.query.unlock_failed);
  const refusedLocal = parseBool(req.query.refused_local_auth_list);
  const chargingLimit = req.query.charging_limit;

  // Faulted-State über Temperatursensor-Relais erzeugen
  if (faulted !== undefined) {
    setRelay(config.relay.tempSensor, !!faulted);
  }

  // unlock_failed / refused_local_auth_list / charging_limit
  // werden aktuell nur geloggt, da dafür keine dedizierte Hardware vorhanden ist.
  if (unlockFailed !== undefined) {
    console.log("  unlock_failed =", unlockFailed, "(nur geloggt)");
  }
  if (refusedLocal !== undefined) {
    console.log("  refused_local_auth_list =", refusedLocal, "(nur geloggt)");
  }
  if (chargingLimit !== undefined) {
    console.log("  charging_limit =", chargingLimit, "(nur geloggt)");
  }

  res.sendStatus(200);
});

// Health-Check für OCTT / manuelle Tests
app.get("/health", (req, res) => {
  const now = Date.now();
  const last = lastStatus ? now - lastStatus.ts : null;
  res.json({
    ok: true,
    coreS3Host: config.coreS3Host,
    lastStatusAgeMs: last,
    hasStatus: !!lastStatus
  });
});

app.listen(config.listenPort, () => {
  console.log(`OCTT SUT Gateway läuft auf Port ${config.listenPort}`);
  console.log(`CoreS3 erwartet unter ${config.coreS3Host} (CMD=${config.coreS3CmdPort}, STATUS=${config.coreS3StatusPort})`);
});


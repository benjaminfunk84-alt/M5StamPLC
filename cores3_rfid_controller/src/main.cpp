// M5CoreS3 RFID-Controller – WiFi SoftAP + UDP (kein externer Router)
// RFID2 (I2C 0x28) = nur Lesen. CoreS3 ersetzt das Phoenix RFID/NFC-Modul (1391227) und hängt direkt am CHARX 3150 (RS-485).
// CHARX 3150 = Master, pollt den „Reader“; CoreS3 antwortet mit der per write_tag (Tab5) gesetzten UID. Partner: M5Tab5.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <M5Unified.h>
#include <MFRC522v2.h>
#include <MFRC522DriverI2C.h>
#include <Adafruit_PN532.h>

// ============================================
// PIN CONFIGURATION
// ============================================
// PORT.A I2C: SDA=21, SCL=22 (Wire1 – vermeidet Konflikt mit M5Unified intern)
#define I2C_SDA  21
#define I2C_SCL  22
#define USE_WIRE1_FOR_I2C  1

// PN532 (Card Emulation, optional): IRQ und RST an freie GPIOs
#define PN532_IRQ  4
#define PN532_RST  5

// RS-485: CoreS3 ersetzt Phoenix-Reader, direkt am CHARX 3150. Port C – RX=16, TX=17.
// M5Stack „RS485-TTL-Konverter Unit“ (U034): Port C = GND, 5V, RX (Yellow), TX (White) → 16/17.
#define USE_RS485_PHOENIX  1
#define RS485_RX_PIN       16
#define RS485_TX_PIN       17
#define RS485_BAUD         9600   // 9,6–115,2 kBit/s (laut CHARX/Reader)
#define RS485_FRAME_MS     150    // Pause = Ende Anfrage vom CHARX, danach Antwort senden
#define RS485_DE_PIN       0      // optional: GPIO für DE/RE (0 = nicht verwendet, bei Half-Duplex setzen)

// CHARX-Steuerung per REST abfragen (laut documentation_rest_mqtt.html: GET .../data?param_list=rfid)
// Nur wenn Steuerung im gleichen Netz erreichbar (CoreS3 AP 192.168.4.x oder CoreS3 im STA-Modus)
#define USE_CHARX_REST_POLL  0    // 1 = aktivieren und URL/DEVICE_UID unten setzen
#define CHARX_REST_URL       "http://192.168.4.2:5555"  // Basis-URL der CHARX-Steuerung (Port laut Doku 5555)
#define CHARX_DEVICE_UID     "py5guu"                   // device_uid des Ladecontrollers mit RFID-Reader
#define CHARX_POLL_INTERVAL_MS  3000

// I2C-Adressen
#define INA226_SHUNT_OHM  0.1f
#define RELAY_I2C_ADDR    0x26   // 4Relay Modul
#define RELAY_REG         0x10
#define LED_REG           0x11
#define RFID2_I2C_ADDR    0x28   // M5Stack RFID2 / WS1850S – nur Lesen (Scan, Speichern)
// RC522 I2C – Schreib-Leser am CHARX RFID/NFC (1391227); typische Adressen 0x23, 0x24, …
#define RC522_I2C_ADDR    0x23
#define RC522_I2C_ADDR_2  0x24
#define RC522_I2C_ADDR_3  0x27
#define RC522_I2C_ADDR_4  0x29
#define PN532_I2C_ADDR    0x24

uint8_t INA226_ADDR = 0;
bool relayPresent     = false;
bool rfid2Present     = false;   // RFID2 = nur Lesen (Scan, Speichern am Tab5)
bool rc522RemotePresent = false; // RC522 = nur Schreiben (CHARX/Phoenix 1391227)
uint8_t rc522RemoteAddr  = 0;    // 0x24 oder 0x29 – welche Adresse beim Scan gefunden
bool pn532Present     = false;

// ============================================
// CONSTANTS
// ============================================
const unsigned long STATUS_SEND_INTERVAL_MS = 500;
const unsigned long INA_READ_INTERVAL_MS   = 4000;
const unsigned long RELAY_READ_INTERVAL_MS  = 4000;
const unsigned long I2C_BUS_PAUSE_MS       = 150;
const unsigned long RFID_TIMEOUT_MS        = 5000;
const size_t        UDP_PAYLOAD_MAX        = 512;

static const char*  AP_SSID         = "CoreS3-AP";
static const char*  AP_PASS         = "cores3pass";
static const int    UDP_STATUS_PORT = 4211;
static const int    UDP_CMD_PORT    = 4210;

WiFiUDP udpStatus;
WiFiUDP udpCmd;

void handleCommandFromTab(const String &jsonStr);

#if USE_RS485_PHOENIX
// RS-485: CoreS3 = Ersatz für Phoenix-Reader am CHARX 3150. Empfang = Anfrage vom CHARX, Sendung = UID-Antwort.
#define RS485_BUF_SIZE  128
static uint8_t rs485Buf[RS485_BUF_SIZE];
static int     rs485Len = 0;
static unsigned long rs485LastByteMs = 0;
// UID, die der CHARX bei der nächsten Abfrage „sieht“ (gesetzt durch write_tag vom Tab5)
static String charxPresentUid = "";
#endif

// Gemeinsame I2C-Instanz für alle externen Module (RFID2, RC522, PN532, INA226, 4Relay).
// Bei USE_WIRE1_FOR_I2C=1 wird Wire1 (Port A 21/22) verwendet,
// sonst der Standardbus Wire (intern, typ. 12/11 – sieht dennoch alle I2C-Geräte inkl. Port A).
#if USE_WIRE1_FOR_I2C
TwoWire& CoreI2C = Wire1;
#else
TwoWire& CoreI2C = Wire;
#endif

// ============================================
// RFID2 – WS1850S (I2C 0x28) für Einlesen/Speichern/Schreiben am Tab5
// ============================================
MFRC522DriverI2C rfid2Driver{RFID2_I2C_ADDR, CoreI2C};
MFRC522 mfrc522{rfid2Driver};

// ============================================
// RC522 I2C – Schreib-Leser am CHARX (Phoenix 1391227); Adresse 0x23, 0x24, …
// ============================================
MFRC522DriverI2C rc522RemoteDriver{RC522_I2C_ADDR, CoreI2C};
MFRC522 mfrc522Remote{rc522RemoteDriver};
MFRC522DriverI2C rc522RemoteDriver2{RC522_I2C_ADDR_2, CoreI2C};
MFRC522 mfrc522Remote2{rc522RemoteDriver2};
MFRC522DriverI2C rc522RemoteDriver3{RC522_I2C_ADDR_3, CoreI2C};
MFRC522 mfrc522Remote3{rc522RemoteDriver3};
MFRC522DriverI2C rc522RemoteDriver4{RC522_I2C_ADDR_4, CoreI2C};
MFRC522 mfrc522Remote4{rc522RemoteDriver4};

static MFRC522* getRc522Remote() {
  if (rc522RemoteAddr == RC522_I2C_ADDR)   return &mfrc522Remote;
  if (rc522RemoteAddr == RC522_I2C_ADDR_2) return &mfrc522Remote2;
  if (rc522RemoteAddr == RC522_I2C_ADDR_3) return &mfrc522Remote3;
  return &mfrc522Remote4;
}

// ============================================
// PN532 – Card Emulation (optional, I2C gleiche Adresse 0x24 möglich)
// ============================================
Adafruit_PN532 pn532(PN532_IRQ, PN532_RST, &CoreI2C);

// ============================================
// GLOBAL VARIABLES
// ============================================
String lastTag = "-";           // vom RFID2 (Einlesen/Speichern)
String lastTagRemote = "-";     // vom RC522 nahe Phoenix
String lastTagPhoenix = "-";    // von CHARX-Steuerung per REST (GET .../data?param_list=rfid)
unsigned long lastTagRefresh = 0;
bool relayState[4] = {false, false, false, false};
bool systemError = false;
unsigned long lastStatusSendMs = 0;
unsigned long lastInaReadMs = 0;
unsigned long lastRelayReadMs = 0;
unsigned long lastDisplayUpdateMs = 0;
int beepFlag = 0;

String rfidTagList[20];
uint8_t rfidTagCount = 0;

static const char* NVS_RFID_NAMESPACE = "rfid";
static const char* NVS_TAG_COUNT_KEY   = "n";
static const char* NVS_TAG_PREFIX     = "t";  // t0, t1, ...

static void loadTagListFromNVS() {
  Preferences prefs;
  if (!prefs.begin(NVS_RFID_NAMESPACE, true)) return;  // read-only
  rfidTagCount = prefs.getUChar(NVS_TAG_COUNT_KEY, 0);
  if (rfidTagCount > 20) rfidTagCount = 20;
  for (uint8_t i = 0; i < rfidTagCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "%s%u", NVS_TAG_PREFIX, i);
    rfidTagList[i] = prefs.getString(key, "");
  }
  prefs.end();
  if (rfidTagCount > 0)
    Serial.printf("RFID: %u Tag(s) aus NVS geladen\n", rfidTagCount);
}

static void saveTagListToNVS() {
  Preferences prefs;
  if (!prefs.begin(NVS_RFID_NAMESPACE, false)) return;  // read-write
  prefs.putUChar(NVS_TAG_COUNT_KEY, rfidTagCount);
  for (uint8_t i = 0; i < rfidTagCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "%s%u", NVS_TAG_PREFIX, i);
    prefs.putString(key, rfidTagList[i]);
  }
  prefs.end();
}

// UID an Gegenstelle (Phoenix CHARX) ausgeben – keine Karte am RC522, nur Übergabe der UID
String  uidToGegenstelle = "";  // Einmalig mit nächstem Status senden (gegenstelle_uid)

// Scan-Modus: nur wenn von Tab5 aktiviert (rfid_scan_start)
bool    rfidScanMode     = false;
unsigned long rfidScanEndMs = 0;

// Write-Fehler: falscher Kartentyp (für Tab5-Anzeige)
bool    writeErrorFlag   = false;
// Write-Erfolg: einmalig an Tab5 senden
bool    writeOkFlag      = false;

// PN532 Card-Emulation: UID für externen Reader (z.B. CHARX)
bool    emulationActive  = false;
String  emulationUid     = "";
unsigned long emulationEndMs = 0;
bool    emulationInited  = false;   // PN532 Target bereits initialisiert?
bool    emulationWasActive = false; // Emulation lief zuvor – nach Ende PN532 resetten
uint32_t emulationInitAttempts = 0; // Debug: Anzahl TgInitAsTarget-Versuche seit Start
uint8_t emulationInitRetryCount = 0; // Anz. Init-Versuche in aktueller Emulation

float cachedVoltage = 0.0f;
float cachedCurrent = 0.0f;

#if USE_WIRE1_FOR_I2C
  TwoWire* i2cBus = &CoreI2C;
#else
  TwoWire* i2cBus = &CoreI2C;
#endif

// ============================================
// I2C / INA226
// ============================================
int16_t readI2CReg16(uint8_t addr, uint8_t reg) {
  i2cBus->beginTransmission(addr);
  i2cBus->write(reg);
  if (i2cBus->endTransmission(false) != 0) return 0;
  if (i2cBus->requestFrom(addr, (uint8_t)2, (uint8_t)true) != 2) return 0;
  uint8_t hi = i2cBus->read();
  uint8_t lo = i2cBus->read();
  return (int16_t)((hi << 8) | lo);
}

uint16_t readI2CRegU16(uint8_t addr, uint8_t reg) {
  i2cBus->beginTransmission(addr);
  i2cBus->write(reg);
  if (i2cBus->endTransmission(false) != 0) return 0;
  if (i2cBus->requestFrom(addr, (uint8_t)2, (uint8_t)true) != 2) return 0;
  uint8_t hi = i2cBus->read();
  uint8_t lo = i2cBus->read();
  return (uint16_t)((hi << 8) | lo);
}

// INA226 Config (0x00): 0x4127 = Continuous, 1.1ms, 16 Samples Avg
static void initINA226() {
  if (INA226_ADDR == 0) return;
  i2cBus->beginTransmission(INA226_ADDR);
  i2cBus->write(0x00);
  i2cBus->write(0x41);
  i2cBus->write(0x27);
  if (i2cBus->endTransmission() != 0) return;
  delay(2);
}

// Liest INA226 an addr; gibt true zurueck wenn Wert plausibel
static bool readINA226At(uint8_t addr, float& outV, float& outI) {
  int16_t shuntRaw = readI2CReg16(addr, 0x01);
  uint16_t busRaw  = readI2CRegU16(addr, 0x02);
  if (busRaw == 0) return false;
  float vBus   = busRaw * 1.25e-3f;  // INA226: alle 16 Bit direkt, kein Shift (nicht INA219)
  float vShunt = shuntRaw * 2.5e-6f;
  if (vBus < 0.5f || vBus > 40.0f) return false;
  outV = vBus;
  outI = vShunt / INA226_SHUNT_OHM;
  return true;
}

void readINA226() {
  // Wenn beim I2C-Scan kein INA226 gefunden wurde (INA226_ADDR == 0),
  // nicht permanent auf 0x40/0x41 herumprobieren – das produziert nur
  // NACKs und kann den I2C-Bus belasten. In dem Fall: einfach nichts tun.
  if (INA226_ADDR == 0) {
    return;
  }
  float v41 = 0, i41 = 0, v40 = 0, i40 = 0;
  bool ok41 = readINA226At(0x41, v41, i41);
  bool ok40 = readINA226At(0x40, v40, i40);
  // Bevorzugt 8..14 V (12V-Bereich) – dann echtes INA226
  if (ok41 && v41 >= 8.0f && v41 <= 14.0f) {
    INA226_ADDR = 0x41;
    cachedVoltage = v41;
    cachedCurrent = i41;
    Serial.printf("INA226 dbg: sel=0x41 v=%.2f i=%.3f (ok41=%d, ok40=%d, v40=%.2f)\n",
                  cachedVoltage, cachedCurrent, ok41, ok40, v40);
    return;
  }
  if (ok40 && v40 >= 8.0f && v40 <= 14.0f) {
    INA226_ADDR = 0x40;
    cachedVoltage = v40;
    cachedCurrent = i40;
    Serial.printf("INA226 dbg: sel=0x40 v=%.2f i=%.3f (ok41=%d, v41=%.2f, ok40=%d)\n",
                  cachedVoltage, cachedCurrent, ok41, v41, ok40);
    return;
  }
  // Sonst hoehere Spannung nehmen (wahrscheinlicher echte Bus-Spannung)
  if (ok41 && ok40) {
    if (v41 >= v40) {
      INA226_ADDR = 0x41;
      cachedVoltage = v41;
      cachedCurrent = i41;
      Serial.printf("INA226 dbg: both ok, choose 0x41 v=%.2f i=%.3f (v40=%.2f)\n",
                    cachedVoltage, cachedCurrent, v40);
    } else {
      INA226_ADDR = 0x40;
      cachedVoltage = v40;
      cachedCurrent = i40;
      Serial.printf("INA226 dbg: both ok, choose 0x40 v=%.2f i=%.3f (v41=%.2f)\n",
                    cachedVoltage, cachedCurrent, v41);
    }
    return;
  }
  if (ok41) {
    INA226_ADDR = 0x41;
    cachedVoltage = v41;
    cachedCurrent = i41;
    Serial.printf("INA226 dbg: only 0x41 ok v=%.2f i=%.3f\n",
                  cachedVoltage, cachedCurrent);
  } else if (ok40) {
    INA226_ADDR = 0x40;
    cachedVoltage = v40;
    cachedCurrent = i40;
    Serial.printf("INA226 dbg: only 0x40 ok v=%.2f i=%.3f\n",
                  cachedVoltage, cachedCurrent);
  } else {
    Serial.println("INA226 dbg: neither 0x41 nor 0x40 returned valid data");
  }
}

// ============================================
// 4Relay (I2C 0x26, Register 0x11: Bits 0–3)
// ============================================
void setRelay(uint8_t idx, bool state) {
  if (idx >= 4) return;
  // Wenn beim I2C-Scan kein 4Relay-Modul gefunden wurde, nicht versuchen,
  // über I2C zu schreiben – das erzeugt nur NACKs und kann den Bus blockieren.
  if (!relayPresent) {
    Serial.printf("setRelay(%u,%u): 4Relay nicht gefunden – kein I2C-Write\n",
                  (unsigned)idx, (unsigned)state);
    relayState[idx] = state;
    return;
  }
  relayState[idx] = state;
  uint8_t val = 0;
  for (int i = 0; i < 4; i++) if (relayState[i]) val |= (1 << i);
  // REG 0x10 aktiv-high: bit=1 → Relais erregt (NO geschlossen)
  i2cBus->beginTransmission(RELAY_I2C_ADDR);
  i2cBus->write(RELAY_REG);
  i2cBus->write(val);
  i2cBus->endTransmission();
  i2cBus->beginTransmission(RELAY_I2C_ADDR);
  i2cBus->write(LED_REG);
  i2cBus->write(val);
  i2cBus->endTransmission();
}

void readRelayState() {
  if (!relayPresent) return;
  i2cBus->beginTransmission(RELAY_I2C_ADDR);
  i2cBus->write(RELAY_REG);
  if (i2cBus->endTransmission(false) != 0) return;
  if (i2cBus->requestFrom((uint16_t)RELAY_I2C_ADDR, (uint8_t)1, (uint8_t)1) != 1) return;
  uint8_t val = i2cBus->read();
  for (int i = 0; i < 4; i++) relayState[i] = (val & (1 << i)) != 0;
}

// ============================================
// WiFi SoftAP + UDP
// ============================================
void setupWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);
  Serial.printf("SoftAP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  udpStatus.begin(UDP_STATUS_PORT);
  udpCmd.begin(UDP_CMD_PORT);
  loadTagListFromNVS();  // Tags nach Neustart wiederherstellen
  Serial.println("UDP bereit (Status:4211 Cmd:4210)");
}

void receiveCommandsUdp() {
  int n = udpCmd.parsePacket();
  if (n <= 0 || n > (int)UDP_PAYLOAD_MAX) return;
  IPAddress remote = udpCmd.remoteIP();
  uint16_t rport   = udpCmd.remotePort();
  char buf[UDP_PAYLOAD_MAX + 1];
  int r = udpCmd.read(buf, UDP_PAYLOAD_MAX);
  if (r <= 0) return;
  buf[r] = '\0';
  Serial.printf("UDP CMD von %s:%u: %s\n",
                remote.toString().c_str(),
                (unsigned)rport,
                buf);
  handleCommandFromTab(String(buf));
}

void sendStatusUdp() {
  JsonDocument doc;
  doc["u"] = cachedVoltage;
  doc["i"] = cachedCurrent;
  doc["rfid"] = lastTag;
  doc["rfid_remote"] = lastTagRemote;
  if (lastTagPhoenix != "-") doc["rfid_phoenix"] = lastTagPhoenix;
  JsonArray tagArray = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < rfidTagCount; i++)
    tagArray.add(rfidTagList[i]);
  doc["err"] = systemError;
  doc["wrerr"] = writeErrorFlag ? 1 : 0;
  writeErrorFlag = false;
  doc["wrok"] = writeOkFlag ? 1 : 0;
  writeOkFlag = false;
  JsonArray relayArray = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 4; i++)
    relayArray.add(relayState[i] ? 1 : 0);
  doc["beep"] = beepFlag;
  doc["pn532"] = pn532Present ? 1 : 0;
  doc["rc522"] = (rc522RemotePresent || (USE_RS485_PHOENIX)) ? 1 : 0;  // Schreiben: RC522 oder RS-485
  doc["charx_rs485"] = (USE_RS485_PHOENIX) ? 1 : 0;  // UID direkt an CHARX 3150 per RS-485 sendbar
  if (uidToGegenstelle.length() > 0) {
    doc["gegenstelle_uid"] = uidToGegenstelle;
    uidToGegenstelle = "";
  }
  doc["emul"] = emulationActive ? 1 : 0;

  char out[UDP_PAYLOAD_MAX];
  size_t n = serializeJson(doc, out, sizeof(out));
  if (n == 0 || n >= UDP_PAYLOAD_MAX) return;

  IPAddress bcast(192, 168, 4, 255);
  udpStatus.beginPacket(bcast, UDP_STATUS_PORT);
  udpStatus.write((const uint8_t*)out, n);
  udpStatus.endPacket();
}

// ============================================
// Commands (wie bisher)
// ============================================
void handleCommandFromTab(const String &jsonStr) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.print("JSON Parse Error: ");
    Serial.println(err.f_str());
    return;
  }
  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "set_relay") == 0) {
    int idx = doc["idx"] | -1;
    int val = doc["val"] | 0;
    if (idx >= 0 && idx < 4) {
      setRelay((uint8_t)idx, (val != 0));
      Serial.printf("Relay %d set to %d\n", idx, relayState[idx] ? 1 : 0);
    }
  }
  else if (strcmp(cmd, "rfid_scan_start") == 0) {
    rfidScanMode   = true;
    rfidScanEndMs  = millis() + 6000;  // 6s (etwas länger als Tab5's 5s)
    lastTag        = "-";
    lastTagRefresh = 0;
    Serial.println("Scan-Modus gestartet");
  }
  else if (strcmp(cmd, "rfid_scan_stop") == 0) {
    rfidScanMode = false;
    lastTag      = "-";
    Serial.println("Scan-Modus gestoppt");
  }
  else if (strcmp(cmd, "rfid_learn") == 0) {
    if (lastTag != "-" && lastTag.length() > 0 && rfidTagCount < 20) {
      bool found = false;
      for (uint8_t i = 0; i < rfidTagCount; i++) {
        if (rfidTagList[i] == lastTag) { found = true; break; }
      }
      if (!found) {
        rfidTagList[rfidTagCount++] = lastTag;
        saveTagListToNVS();
        Serial.printf("RFID learned: %s (total: %d)\n", lastTag.c_str(), rfidTagCount);
        beepFlag = 1;
      }
    }
  }
  else if (strcmp(cmd, "rfid_delete") == 0) {
    const char* uid = doc["uid"];
    if (uid) {
      String uidStr = String(uid);
      for (uint8_t i = 0; i < rfidTagCount; i++) {
        if (rfidTagList[i] == uidStr) {
          for (uint8_t j = i; j < rfidTagCount - 1; j++)           rfidTagList[j] = rfidTagList[j + 1];
          rfidTagCount--;
          saveTagListToNVS();
          Serial.printf("RFID deleted: %s (remaining: %d)\n", uid, rfidTagCount);
          break;
        }
      }
    }
  }
  else if (strcmp(cmd, "write_tag") == 0) {
    const char* uid = doc["uid"];
    const char* target = doc["target"].as<const char*>();
    if (!target) target = "";
    bool wantPn532 = (strcmp(target, "pn532") == 0) || (target[0] == '\0' && pn532Present);
    bool wantRs485 = (strcmp(target, "rs485") == 0) || (target[0] == '\0' && USE_RS485_PHOENIX);
    if (uid && (rc522RemotePresent || pn532Present || (USE_RS485_PHOENIX))) {
      uidToGegenstelle = String(uid);
      writeOkFlag      = true;
      beepFlag         = 1;
#if USE_RS485_PHOENIX
      if (wantRs485) {
        charxPresentUid = String(uid);
        Serial.printf("UID an CHARX 3150 (RS-485): %s\n", uid);
      }
#endif
      if (wantPn532 && pn532Present) {
        emulationUid    = String(uid);
        emulationActive = true;
        emulationInited = false;             // bei jedem Senden neu initialisieren
        emulationInitRetryCount = 0;         // Retry-Zähler für diese Emulation zurücksetzen
        emulationEndMs  = millis() + 3000;   // 3 s – kurz wie Karte anhalten
        Serial.printf("UID PN532-Emulation: %s (3 s)\n", uid);
      }
      if (!wantPn532 && !wantRs485)
        Serial.printf("UID gesetzt (target=%s)\n", target);
    }
  }
  else if (strcmp(cmd, "rfid_emulate") == 0) {
    const char* uid = doc["uid"];
    if (uid && pn532Present) {
      emulationUid     = String(uid);
      emulationActive  = true;
      emulationInited  = false;              // erneutes Emulate vom Tab erzwingt neue Init
      emulationInitRetryCount = 0;           // Retry-Zähler für diese Emulation zurücksetzen
      emulationEndMs   = millis() + 3000;    // 3 s – kurz wie Karte anhalten
      Serial.printf("Emulation: UID %s (3 s)\n", uid);
    }
  }
  else if (strcmp(cmd, "rfid_emulate_stop") == 0) {
    emulationActive = false;
    Serial.println("Emulation gestoppt");
  }
  else if (strcmp(cmd, "rfid_play") == 0) {
    const char* tag = doc["tag"];
    if (tag && lastTag == String(tag)) {
      beepFlag = 1;
      Serial.printf("RFID matched: %s\n", tag);
    }
  }
}

// ============================================
// RFID2 – UID auslesen (nur für Einlesen/Speichern/Schreiben am Tab5)
// Hardware-UID (Chip-UID) in der vom MFRC522 gelieferten Reihenfolge (z. B. DA2E72B1).
// ============================================
static String uidToHexRaw(MFRC522* picc) {
  String s = "";
  for (byte i = 0; i < picc->uid.size; i++) {
    char h[3]; snprintf(h, 3, "%02X", picc->uid.uidByte[i]);
    s += h;
  }
  return s;
}

static String uidToHexMsbFirst(MFRC522* picc) {
  String s = "";
  for (int i = (int)picc->uid.size - 1; i >= 0; i--) {
    char h[3]; snprintf(h, 3, "%02X", picc->uid.uidByte[i]);
    s += h;
  }
  return s;
}

#ifndef USE_UID_REVERSED
#define USE_UID_REVERSED 0
#endif
static String uidForSystem(MFRC522* picc) {
  return USE_UID_REVERSED ? uidToHexMsbFirst(picc) : uidToHexRaw(picc);
}

String readRFID2() {
  if (!rfid2Present) return "";
  if (!mfrc522.PICC_IsNewCardPresent()) return "";
  if (!mfrc522.PICC_ReadCardSerial())   return "";

  String hwUid = uidForSystem(&mfrc522);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  Serial.printf("RFID2 Tag: %s\n", hwUid.c_str());
  return hwUid;
}

// ============================================
// PN532 – Card Emulation (ISO 14443-A) mit konfigurierbarer UID
// Phoenix Contact Reader (13,56 MHz, ISO 14443-A/15693) erkennt Tag über ATQA, UID, SAK.
// TgInitAsTarget: SENS_RES (ATQA) + NFCID1t (erste 3 UID-Bytes) + SEL_RES (SAK) laut ISO 14443-A.
// MIFARE Classic 1k: ATQA = 0x04 0x00, SAK = 0x08 (4-Byte-UID).
// ============================================
static bool pn532InitAsTargetWithUid(const String &uidStr) {
  if (uidStr.length() < 8) {
    Serial.println("PN532 Init: UID zu kurz");
    return false;  // mind. 4 Byte Hex = 8 Zeichen
  }
  emulationInitAttempts++;
  Serial.printf("PN532 Init: Versuch %lu mit UID %s\n",
                static_cast<unsigned long>(emulationInitAttempts),
                uidStr.c_str());
  pn532.SAMConfig();  // PN532 vor Target-Modus in definierten Zustand
  uint8_t uid4[4];
  for (int i = 0; i < 4; i++) {
    char hex[3] = { uidStr.charAt(i*2), uidStr.charAt(i*2+1), '\0' };
    uid4[i] = (uint8_t)strtoul(hex, nullptr, 16);
  }
  delay(30);  // Kurz warten, damit I2C-Bus bereit ist (Senden bei jedem Klick zuverlaessig)
  // TgInitAsTarget: Mode 0 = ISO/IEC 14443A. MifareParams: SENS_RES(2), NFCID1t(3), SEL_RES(1)
  uint8_t target[] = {
    0x8C, 0x00,                           // Command, Mode 14443A
    0x04, 0x00,                           // SENS_RES (ATQA) MIFARE Classic 4-byte UID
    uid4[0], uid4[1], uid4[2],            // NFCID1t (4. Byte = BCC wird bei Anti-Collision ergänzt)
    0x08,                                 // SEL_RES (SAK) MIFARE Classic 1k
    0x01, 0xfe,                           // FeliCa POL_RES (NFCID2t Start)
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xc0,
    0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xff, 0xff,
    0xaa, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44,
    0x33, 0x22, 0x11, 0x01, 0x00,
    0x0d, 0x52, 0x46, 0x49, 0x44, 0x49, 0x4f,
    0x74, 0x20, 0x50, 0x4e, 0x35, 0x33, 0x32
  };
  if (!pn532.sendCommandCheckAck(target, sizeof(target))) {
    Serial.printf("PN532 TgInitAsTarget fehlgeschlagen (Versuch %lu)\n",
                  static_cast<unsigned long>(emulationInitAttempts));
    return false;
  }
  Serial.printf("PN532 TgInitAsTarget OK (Versuch %lu)\n",
                static_cast<unsigned long>(emulationInitAttempts));
  return true;
}

#if USE_CHARX_REST_POLL
// CHARX-Steuerung: GET /api/v1.0/charging-controllers/{{DEVICE_UID}}/data?param_list=rfid
// Response: { "rfid": { "tag": "12423445243576573423", "timestamp": "..." } }
static void pollCharxRfidRest() {
  HTTPClient http;
  String url = String(CHARX_REST_URL) + "/api/v1.0/charging-controllers/" + CHARX_DEVICE_UID + "/data?param_list=rfid";
  http.begin(url);
  http.setTimeout(2000);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok) {
      const char* tag = doc["rfid"]["tag"];
      if (tag && strlen(tag) > 0)
        lastTagPhoenix = String(tag);
      else
        lastTagPhoenix = "-";
    }
  } else {
    http.end();
    lastTagPhoenix = "-";
  }
}
#endif

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\n=== M5CoreS3 RFID Controller (ESP-NOW) ===");

  auto cfg = M5.config();
  cfg.clear_display = true;
  M5.begin(cfg);
  M5.Display.setRotation(1);   // 90° nach rechts
  M5.Display.setTextSize(2);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setCursor(0, 0);
  M5.Display.println("CoreS3 RFID");
  M5.Display.println("ESP-NOW");
  M5.Display.println("Starting...");
  M5.Display.setBrightness(128);  // Nach Reset Display wieder an (vermeidet schwarzen Bildschirm)
  delay(200);

#if USE_WIRE1_FOR_I2C
  // Kein end(): auf CoreS3 führt Wire1.end() zu "Invalid pin: 22", danach funktioniert Wire1 nicht mehr.
  // Bei board:10 ist Wire1 bereits 21/22; nur begin() + setClock/setTimeOut.
  i2cBus->begin(I2C_SDA, I2C_SCL);
  i2cBus->setClock(100000);
  i2cBus->setTimeOut(1000);  // 1 s I2C-Timeout, verhindert Hänger in pn532.begin()
  // #region agent log
  Serial.printf("I2C bus=Wire1 SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  // #endregion
#else
  // Für Wire (Bus 0) die von M5Unified / Arduino vordefinierten Pins verwenden (kein eigenes Pin-Mapping),
  // damit der Bus auch dann funktioniert, wenn M5GFX das Board falsch erkennt (board:0).
  i2cBus->begin();  // Default-Pins beibehalten (typ. 12/11 auf CoreS3)
  i2cBus->setClock(100000);
  i2cBus->setTimeOut(1000);
  // #region agent log
  Serial.println("I2C bus=Wire (default pins)");
  // #endregion
#endif
  Serial.println("I2C-Scan 0x08..0x77:");
  String foundAddr = "";
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    i2cBus->beginTransmission(a);
    if (i2cBus->endTransmission() == 0) {
      if (foundAddr.length()) foundAddr += ", ";
      foundAddr += "0x";
      char h[3]; snprintf(h, 3, "%02X", a);
      foundAddr += h;
      Serial.printf("  0x%02X\n", a);
      if (a == RELAY_I2C_ADDR)   relayPresent = true;
      if (a == RFID2_I2C_ADDR)   rfid2Present = true;
      if (a == RC522_I2C_ADDR)   { rc522RemotePresent = true; rc522RemoteAddr = RC522_I2C_ADDR; }
      if (a == RC522_I2C_ADDR_2 && !rc522RemotePresent) { rc522RemotePresent = true; rc522RemoteAddr = RC522_I2C_ADDR_2; }
      if (a == RC522_I2C_ADDR_3 && !rc522RemotePresent) { rc522RemotePresent = true; rc522RemoteAddr = RC522_I2C_ADDR_3; }
      if (a == RC522_I2C_ADDR_4 && !rc522RemotePresent) { rc522RemotePresent = true; rc522RemoteAddr = RC522_I2C_ADDR_4; }
      if (a == 0x41) INA226_ADDR = 0x41;
      else if (a == 0x40 && INA226_ADDR == 0) INA226_ADDR = 0x40;
    }
  }
  Serial.printf("I2C gefunden: %s\n", foundAddr.length() ? foundAddr.c_str() : "(keine)");
  M5.Display.println("I2C OK");
  if (!rc522RemotePresent && foundAddr.length())
    Serial.println("RC522 nicht bei 0x23/0x24/0x27/0x29 – Adresse aus Liste oben? Dann in main.cpp RC522_I2C_ADDR setzen.");

  // PN532 an I2C (typ. 0x24): nur versuchen wenn 0x24 im Scan war, mit Timeout gegen Hänger
  bool tryPn532 = (foundAddr.indexOf("0x24") >= 0 || foundAddr.indexOf("24") >= 0);
  M5.Display.println("PN532...");
  if (tryPn532 && pn532.begin()) {
    pn532.SAMConfig();
    pn532Present = true;
    if (rc522RemoteAddr == PN532_I2C_ADDR) {
      rc522RemotePresent = false;
      rc522RemoteAddr = 0;
    }
    Serial.println("PN532 an I2C gefunden und bereit (Emulation)");
    Serial.println("PN532 erkannt: JA - Karte an Reader halten fuer Emulation");
    // Kein i2cBus->end()/begin(): auf CoreS3 führt das zu "Invalid pin: 22" und zerstört Wire1
  }
  // Längeren Timeout beibehalten, damit PN532-Emulation (TgInitAsTarget / getDataTarget)
  // nicht permanent in I2C-Timeouts (ESP_ERR_INVALID_STATE) läuft.
  i2cBus->setTimeOut(1000);

  if (INA226_ADDR != 0) {
    Serial.printf("INA226 gefunden: 0x%02X\n", INA226_ADDR);
    initINA226();
    delay(300);
    float v, ci;
    if (readINA226At(0x41, v, ci)) Serial.printf("INA226 0x41: %.2f V, %.3f A\n", v, ci);
    if (readINA226At(0x40, v, ci)) Serial.printf("INA226 0x40: %.2f V, %.3f A\n", v, ci);
  } else
    Serial.println("INA226: nicht gefunden (0x40/0x41).");
  Serial.printf("4Relay (0x26): %s\n", relayPresent ? "gefunden" : "nicht gefunden");

  Serial.printf("RFID2 (0x28): %s\n", rfid2Present ? "gefunden" : "nicht gefunden");
  if (rfid2Present) {
    mfrc522.PCD_Init();
    delay(50);
    Serial.println("RFID2 bereit (nur Lesen: Scan, Speichern)");
  }

  Serial.printf("RC522 Remote (0x23/0x24/0x27/0x29): %s\n", rc522RemotePresent ? "gefunden" : "nicht gefunden");
  if (rc522RemotePresent) {
    if (rc522RemoteAddr == RC522_I2C_ADDR) {
      mfrc522Remote.PCD_Init();
      Serial.printf("RC522 Schreib-Reader: Adresse 0x%02X (nahe Phoenix)\n", RC522_I2C_ADDR);
    } else if (rc522RemoteAddr == RC522_I2C_ADDR_2) {
      mfrc522Remote2.PCD_Init();
      Serial.printf("RC522 Schreib-Reader: Adresse 0x%02X (nahe Phoenix)\n", RC522_I2C_ADDR_2);
    } else if (rc522RemoteAddr == RC522_I2C_ADDR_3) {
      mfrc522Remote3.PCD_Init();
      Serial.printf("RC522 Schreib-Reader: Adresse 0x%02X (nahe Phoenix)\n", RC522_I2C_ADDR_3);
    } else {
      mfrc522Remote4.PCD_Init();
      Serial.printf("RC522 Schreib-Reader: Adresse 0x%02X (nahe Phoenix)\n", RC522_I2C_ADDR_4);
    }
    delay(50);
  } else {
    Serial.println("Hinweis: Schreiben über PN532 und/oder RS-485 (CHARX). RC522 optional.");
  }

  Serial.printf("PN532: %s\n", pn532Present ? "bereit (Emulation)" : "nicht gefunden");
  if (!pn532Present)
    Serial.println("PN532 erkannt: NEIN - I2C-Adresse 0x24 pruefen, Verkabelung/Wire1");
  if (pn532Present) {
    // Kein Bus-Reset (end/begin): auf CoreS3 zerstört das Wire1, Pin 22 wird "Invalid")
    // Optional: andere I2C-Chips nochmal ansprechen (ohne Bus neu zu starten)
    if (rfid2Present) {
      mfrc522.PCD_Init();
      delay(50);
    }
    if (rc522RemotePresent) {
      if (rc522RemoteAddr == RC522_I2C_ADDR) mfrc522Remote.PCD_Init();
      else if (rc522RemoteAddr == RC522_I2C_ADDR_2) mfrc522Remote2.PCD_Init();
      else if (rc522RemoteAddr == RC522_I2C_ADDR_3) mfrc522Remote3.PCD_Init();
      else mfrc522Remote4.PCD_Init();
      delay(50);
    }
    if (INA226_ADDR != 0) {
      initINA226();
      delay(10);
    }
  }

  if (relayPresent) {
    setRelay(0, false);
    setRelay(1, false);
    setRelay(2, false);
    setRelay(3, false);
    Serial.println("4Relay OK");
  }

#if USE_RS485_PHOENIX
  Serial2.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  if (RS485_DE_PIN != 0) {
    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);  // Empfang
  }
  Serial.printf("RS-485: CoreS3 = Reader-Ersatz für CHARX 3150, Port C RX=%d TX=%d, %u Bd\n",
                 RS485_RX_PIN, RS485_TX_PIN, (unsigned)RS485_BAUD);
#endif

  setupWiFiAP();

  M5.Display.fillScreen(TFT_BLACK);
  Serial.println("Setup complete");
}

// ============================================
// LOOP
// ============================================
void loop() {
  unsigned long now = millis();

#if USE_RS485_PHOENIX
  // RS-485: Anfrage vom CHARX 3150 sammeln, bei Pause als Telegramm loggen und mit UID antworten
  while (Serial2.available()) {
    int b = Serial2.read();
    if (b >= 0 && rs485Len < RS485_BUF_SIZE) {
      rs485Buf[rs485Len++] = (uint8_t)b;
      rs485LastByteMs = now;
    }
  }
  if (rs485Len > 0 && (now - rs485LastByteMs) >= RS485_FRAME_MS) {
    Serial.print("RS-485 CHARX Anfrage (");
    Serial.print(rs485Len);
    Serial.print(" Bytes): ");
    for (int i = 0; i < rs485Len; i++) {
      if (i) Serial.print(' ');
      char h[4];
      snprintf(h, sizeof(h), "%02X", rs485Buf[i]);
      Serial.print(h);
    }
    Serial.println();
    // Antwort an CHARX: UID als 4 Bytes (8 Hex-Zeichen → 4 Byte), falls gesetzt
    if (charxPresentUid.length() >= 8) {
      uint8_t uid4[4];
      for (int i = 0; i < 4; i++) {
        char hex[3] = { charxPresentUid.charAt(i*2), charxPresentUid.charAt(i*2+1), '\0' };
        uid4[i] = (uint8_t)strtoul(hex, nullptr, 16);
      }
#if (RS485_DE_PIN != 0)
      digitalWrite(RS485_DE_PIN, HIGH);
      delay(1);
#endif
      Serial2.write(uid4, 4);
      Serial2.flush();
#if (RS485_DE_PIN != 0)
      delay(1);
      digitalWrite(RS485_DE_PIN, LOW);
#endif
      Serial.printf("RS-485 Antwort: UID %s (4 Bytes)\n", charxPresentUid.c_str());
      // Nur einmal antworten: danach UID loeschen, bis Tab5 erneut write_tag sendet
      charxPresentUid = "";
    }
    rs485Len = 0;
  }
#endif

#if USE_CHARX_REST_POLL
  static unsigned long lastCharxPollMs = 0;
  if (now - lastCharxPollMs >= CHARX_POLL_INTERVAL_MS) {
    lastCharxPollMs = now;
    pollCharxRfidRest();
  }
#endif

  // PN532 Card-Emulation (externer Reader z.B. CHARX / Handy)
  if (!emulationActive) {
    emulationInited = false;
    if (pn532Present && emulationWasActive) {
      pn532.SAMConfig();
      delay(50);
      pn532.SAMConfig();  // Doppelt: Chip zuverlaessig aus Target-Modus holen
      delay(100);         // Kurz warten, bevor naechster TgInitAsTarget erlaubt ist
      emulationWasActive = false;
    }
  }
  if (pn532Present && emulationActive) {
    emulationWasActive = true;
    if (now >= emulationEndMs) {
      emulationActive = false;
      Serial.println("Emulation: Timeout");
    } else {
      if (!emulationInited) {
        const uint8_t MAX_EMUL_INIT_RETRIES = 3;
        if (emulationInitRetryCount >= MAX_EMUL_INIT_RETRIES) {
          Serial.println("PN532 Emulation Init: zu viele Fehlversuche, Abbruch");
          emulationActive = false;
        } else {
          if (pn532InitAsTargetWithUid(emulationUid)) {
            emulationInited = true;
            Serial.printf("PN532 Target: UID %s – Reader in Reichweite halten\n", emulationUid.c_str());
          } else {
            emulationInitRetryCount++;
          }
        }
      }
      if (emulationInited) {
        // Polling alle 60 ms – zu aggressives Polling (25 ms) führt zu I2C-NACK und ESP_ERR_INVALID_STATE
        static uint32_t lastEmulPoll = 0;
        if (now - lastEmulPoll >= 60) {
          lastEmulPoll = now;
          uint8_t cmd[64];
          uint8_t cmdLen;
          if (pn532.getDataTarget(cmd, &cmdLen)) {
            pn532.setDataTarget(cmd, cmdLen);
          }
        }
      }
    }
  }

  // RFID2: Scan nur wenn von Tab5 aktiviert (kein Write-Modus mehr – UID wird nur an Gegenstelle gesendet)
  if (rfidScanMode) {
    if (now >= rfidScanEndMs) {
      rfidScanMode = false;
      lastTag = "-";
      Serial.println("Scan-Modus: Timeout");
    } else {
      static uint32_t lastRfidPollMs = 0;
      if (now - lastRfidPollMs >= 200) {
        lastRfidPollMs = now;
        String uid = readRFID2();
        if (uid.length() > 0) {
          lastTag = uid;
          lastTagRefresh = now;
          Serial.printf("RFID2 Tag erkannt: %s\n", uid.c_str());
        }
      }
    }
    if (now - lastTagRefresh > RFID_TIMEOUT_MS && lastTag != "-") {
      lastTag = "-";
    }
  } else {
    // Kein aktiver Modus: kein Scan, lastTag leer
    if (lastTag != "-") lastTag = "-";
  }

  // RC522 darf unter keinen Umständen lesen – nur Schreiben. Kein Polling, keine UID-Anzeige.
  lastTagRemote = "-";

  // I2C: INA226 lesen – während PN532-Emulation überspringen, um Wire1 nicht zu belasten (vermeidet ESP_ERR_INVALID_STATE)
  if (!emulationActive && now - lastInaReadMs >= INA_READ_INTERVAL_MS) {
    lastInaReadMs = now;
    delay(I2C_BUS_PAUSE_MS);
    readINA226();
    delay(I2C_BUS_PAUSE_MS);
  }

  receiveCommandsUdp();

  if (now - lastStatusSendMs >= STATUS_SEND_INTERVAL_MS) {
    lastStatusSendMs = now;
    sendStatusUdp();
    beepFlag = 0;
  }

  // Display: seltener aktualisieren, mit Hintergrundfarbe zeichnen = weniger Flackern
  const unsigned long DISPLAY_UPDATE_MS = 1200;
  if (now - lastDisplayUpdateMs >= DISPLAY_UPDATE_MS) {
    lastDisplayUpdateMs = now;
    int w = M5.Display.width();
    int h = M5.Display.height();
    M5.Display.fillRect(0, 0, w, h, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.printf("RFID: %s\n", lastTag.c_str());
    M5.Display.printf("Phoenix: %s\n", lastTagRemote.c_str());
    if (INA226_ADDR != 0)
      M5.Display.printf("U: %.2f V  I: %.3f A\n", cachedVoltage, cachedCurrent);
    else
      M5.Display.println("U: --- V  I: --- A (INA N/A)");
    M5.Display.print("R: ");
    for (int i = 0; i < 4; i++) M5.Display.print(relayState[i] ? "1" : "0");
    M5.Display.printf("  Tags:%d\n", rfidTagCount);
    M5.Display.println("(WiFi AP)");
  }

  M5.update();
  delay(10);
}

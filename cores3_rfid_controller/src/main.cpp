// M5CoreS3 RFID-Controller – WiFi SoftAP + UDP (kein externer Router)
// Stack: CoreS3, 4Relay, INA226 (I2C), RFID2/WS1850S (I2C). Partner: M5Tab5.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <M5Unified.h>
#include <MFRC522v2.h>
#include <MFRC522DriverI2C.h>

// ============================================
// PIN CONFIGURATION
// ============================================
// PORT.A I2C: SDA=21, SCL=22 (Wire1 – vermeidet Konflikt mit M5Unified intern)
#define I2C_SDA  21
#define I2C_SCL  22
#define USE_WIRE1_FOR_I2C  1

// I2C-Adressen
#define INA226_SHUNT_OHM  0.1f
#define RELAY_I2C_ADDR    0x26   // 4Relay Modul
#define RELAY_REG         0x10   // Relay-Switch (physisch) – beweis: 0x11 war LED-Register
#define LED_REG           0x11   // LED-Switch (visuelle Anzeige am Modul)
#define RFID2_I2C_ADDR    0x28   // M5Stack RFID2 / WS1850S

uint8_t INA226_ADDR = 0;
bool relayPresent  = false;
bool rfid2Present  = false;

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

// ============================================
// RFID2 – WS1850S via MFRC522v2 Library (I2C)
// ============================================
MFRC522DriverI2C rfid2Driver{RFID2_I2C_ADDR, Wire1};
MFRC522 mfrc522{rfid2Driver};

// ============================================
// GLOBAL VARIABLES
// ============================================
String lastTag = "-";
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

// Write-Modus: nächste Karte am RFID2-Leser beschreiben
bool    rfidWriteMode    = false;
String  rfidWriteTarget  = "";
unsigned long rfidWriteEndMs = 0;

// Scan-Modus: nur wenn von Tab5 aktiviert (rfid_scan_start)
bool    rfidScanMode     = false;
unsigned long rfidScanEndMs = 0;

// Write-Fehler: falscher Kartentyp (für Tab5-Anzeige)
bool    writeErrorFlag   = false;

float cachedVoltage = 0.0f;
float cachedCurrent = 0.0f;

#if USE_WIRE1_FOR_I2C
  TwoWire* i2cBus = &Wire1;
#else
  TwoWire* i2cBus = &Wire;
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
  float v41 = 0, i41 = 0, v40 = 0, i40 = 0;
  bool ok41 = readINA226At(0x41, v41, i41);
  bool ok40 = readINA226At(0x40, v40, i40);
  // Bevorzugt 8..14 V (12V-Bereich) – dann echtes INA226
  if (ok41 && v41 >= 8.0f && v41 <= 14.0f) {
    INA226_ADDR = 0x41;
    cachedVoltage = v41;
    cachedCurrent = i41;
    return;
  }
  if (ok40 && v40 >= 8.0f && v40 <= 14.0f) {
    INA226_ADDR = 0x40;
    cachedVoltage = v40;
    cachedCurrent = i40;
    return;
  }
  // Sonst hoehere Spannung nehmen (wahrscheinlicher echte Bus-Spannung)
  if (ok41 && ok40) {
    if (v41 >= v40) {
      INA226_ADDR = 0x41;
      cachedVoltage = v41;
      cachedCurrent = i41;
    } else {
      INA226_ADDR = 0x40;
      cachedVoltage = v40;
      cachedCurrent = i40;
    }
    return;
  }
  if (ok41) {
    INA226_ADDR = 0x41;
    cachedVoltage = v41;
    cachedCurrent = i41;
  } else if (ok40) {
    INA226_ADDR = 0x40;
    cachedVoltage = v40;
    cachedCurrent = i40;
  }
}

// ============================================
// 4Relay (I2C 0x26, Register 0x11: Bits 0–3)
// ============================================
void setRelay(uint8_t idx, bool state) {
  if (idx >= 4) return;
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
  Serial.println("UDP bereit (Status:4211 Cmd:4210)");
}

void receiveCommandsUdp() {
  int n = udpCmd.parsePacket();
  if (n <= 0 || n > (int)UDP_PAYLOAD_MAX) return;
  char buf[UDP_PAYLOAD_MAX + 1];
  int r = udpCmd.read(buf, UDP_PAYLOAD_MAX);
  buf[r] = '\0';
  handleCommandFromTab(String(buf));
}

void sendStatusUdp() {
  JsonDocument doc;
  doc["u"] = cachedVoltage;
  doc["i"] = cachedCurrent;
  doc["rfid"] = lastTag;
  JsonArray tagArray = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < rfidTagCount; i++)
    tagArray.add(rfidTagList[i]);
  doc["err"] = systemError;
  doc["wrerr"] = writeErrorFlag ? 1 : 0;
  writeErrorFlag = false;  // Einmalig senden, dann zurücksetzen
  JsonArray relayArray = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 4; i++)
    relayArray.add(relayState[i] ? 1 : 0);
  doc["beep"] = beepFlag;

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
          for (uint8_t j = i; j < rfidTagCount - 1; j++) rfidTagList[j] = rfidTagList[j + 1];
          rfidTagCount--;
          Serial.printf("RFID deleted: %s (remaining: %d)\n", uid, rfidTagCount);
          break;
        }
      }
    }
  }
  else if (strcmp(cmd, "write_tag") == 0) {
    const char* uid = doc["uid"];
    if (uid && rfid2Present) {
      rfidWriteMode   = true;
      rfidWriteTarget = String(uid);
      rfidWriteEndMs  = millis() + 10000;
      Serial.printf("Write-Modus: warte auf Karte fuer UID %s\n", uid);
    }
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
// RFID2 – UID auslesen via MFRC522v2
// ============================================
String readRFID2() {
  if (!rfid2Present) return "";
  if (!mfrc522.PICC_IsNewCardPresent()) return "";
  if (!mfrc522.PICC_ReadCardSerial())   return "";
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    char h[3]; snprintf(h, 3, "%02X", mfrc522.uid.uidByte[i]);
    uid += h;
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  return uid;
}

// ============================================
// RFID2 – Daten auf MIFARE-Karte schreiben
// Schreibt rfidWriteTarget (UID-String) in Block 1 (Sektor 0)
// mit Default-Key A (FF FF FF FF FF FF)
// ============================================
bool writeUidToCard() {
  if (!rfid2Present) { Serial.println("[WR] rfid2 not present"); return false; }
  // #region agent log write-debug
  if (!mfrc522.PICC_IsNewCardPresent()) { Serial.println("[WR] no new card"); return false; }
  Serial.println("[WR] card present");
  if (!mfrc522.PICC_ReadCardSerial()) { Serial.println("[WR] ReadSerial failed"); return false; }
  Serial.printf("[WR] card UID read: %d bytes, type=%d\n",
                mfrc522.uid.size, (int)mfrc522.PICC_GetType(mfrc522.uid.sak));
  // #endregion

  // Nur MIFARE Classic unterstützt (type 3=Mini, 4=1K, 5=4K)
  MFRC522Constants::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  bool isMifareClassic = (piccType == MFRC522Constants::PICC_TYPE_MIFARE_MINI ||
                          piccType == MFRC522Constants::PICC_TYPE_MIFARE_1K   ||
                          piccType == MFRC522Constants::PICC_TYPE_MIFARE_4K);
  // #region agent log write-debug
  Serial.printf("[WR] piccType=%d isMifare=%d\n", (int)piccType, (int)isMifareClassic);
  // #endregion
  if (!isMifareClassic) {
    Serial.printf("[WR] Karte nicht MIFARE Classic (type=%d) - falscher Kartentyp!\n", (int)piccType);
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    writeErrorFlag = true;  // Tab5 informieren
    return false;
  }

  MFRC522Constants::MIFARE_Key key;
  for (byte i = 0; i < MFRC522Constants::MIFARE_Misc::MF_KEY_SIZE; i++) key.keyByte[i] = 0xFF;

  const byte blockAddr = 1;

  MFRC522Constants::StatusCode authStatus = mfrc522.PCD_Authenticate(
      MFRC522Constants::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &mfrc522.uid);
  // #region agent log write-debug
  Serial.printf("[WR] auth status=%d (0=OK)\n", (int)authStatus);
  // #endregion
  if (authStatus != MFRC522Constants::STATUS_OK) {
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return false;
  }

  byte buf[16] = {0};
  const char* src = rfidWriteTarget.c_str();
  size_t len = min((size_t)16, strlen(src));
  memcpy(buf, src, len);

  MFRC522Constants::StatusCode writeStatus = mfrc522.MIFARE_Write(blockAddr, buf, 16);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  // #region agent log write-debug
  Serial.printf("[WR] write status=%d target=\"%s\"\n", (int)writeStatus, rfidWriteTarget.c_str());
  // #endregion
  if (writeStatus == MFRC522Constants::STATUS_OK) {
    Serial.printf("Write OK: \"%s\"\n", rfidWriteTarget.c_str());
    return true;
  }
  return false;
}

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
  delay(200);

#if USE_WIRE1_FOR_I2C
  i2cBus->begin(I2C_SDA, I2C_SCL);
  i2cBus->setClock(100000);
  Serial.printf("I2C Wire1 SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
#else
  i2cBus->begin(I2C_SDA, I2C_SCL);
  i2cBus->setClock(100000);
#endif
  Serial.println("I2C-Scan 0x08..0x77:");
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    i2cBus->beginTransmission(a);
    if (i2cBus->endTransmission() == 0) {
      Serial.printf("  0x%02X\n", a);
      if (a == RELAY_I2C_ADDR)  relayPresent = true;
      if (a == RFID2_I2C_ADDR)  rfid2Present = true;
      if (a == 0x41) INA226_ADDR = 0x41;
      else if (a == 0x40 && INA226_ADDR == 0) INA226_ADDR = 0x40;
    }
  }
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
    Serial.println("RFID2 bereit");
  }

  if (relayPresent) {
    setRelay(0, false);
    setRelay(1, false);
    setRelay(2, false);
    setRelay(3, false);
    Serial.println("4Relay OK");
  }

  setupWiFiAP();

  M5.Display.fillScreen(TFT_BLACK);
  Serial.println("Setup complete");
}

// ============================================
// LOOP
// ============================================
void loop() {
  unsigned long now = millis();

  // RFID2: Write-Modus hat Vorrang; Scan nur wenn von Tab5 aktiviert
  if (rfidWriteMode) {
    if (now >= rfidWriteEndMs) {
      rfidWriteMode = false;
      lastTag = "-";
      Serial.println("Write-Modus: Timeout");
    } else {
      // #region agent log write-debug
      static uint32_t lastWriteLog = 0;
      if (now - lastWriteLog > 2000) { lastWriteLog = now;
        Serial.printf("[WR] write-mode aktiv, target=\"%s\"\n", rfidWriteTarget.c_str()); }
      // #endregion
      bool ok = writeUidToCard();
      if (ok) {
        rfidWriteMode = false;
        beepFlag = 1;
        lastTag = "-";
      }
    }
  } else if (rfidScanMode) {
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

  // I2C: nur INA226 lesen; readRelayState() entfernt da raw=0xFF unreliable
  if (now - lastInaReadMs >= INA_READ_INTERVAL_MS) {
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

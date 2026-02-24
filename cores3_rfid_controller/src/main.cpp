// M5CoreS3 RFID-Controller – Kommunikation per ESP-NOW (direkt, kein WLAN)
// Stack: CoreS3, 4Relay, INA226 (I2C), UHF-RFID (UART). Partner: M5Tab5.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_now.h>
#include <M5Unified.h>

// ============================================
// PIN CONFIGURATION – laut Aufdruck auf der Base
// ============================================
// PORT.A.I2C: SDA=21, SCL=22 (siehe Bild)
// PORT.C.UART: RX=13, TX=14 (siehe Bild)
#define USE_CATM_MODULE_PINS  1

#if USE_CATM_MODULE_PINS
  // Base-Aufdruck: PORT.A.I2C = 22 SCL, 21 SDA
  #define I2C_SDA  21
  #define I2C_SCL  22
  #define USE_WIRE1_FOR_I2C  1
  // M5Stack CoreS3 Standard Port C: RX=18, TX=17 (bestaetigt durch UART-Scanner)
  #define RFID_RX_PIN  18
  #define RFID_TX_PIN  17
#else
  #define I2C_SDA  2
  #define I2C_SCL  1
  #define USE_WIRE1_FOR_I2C  0
  #define RFID_RX_PIN  18
  #define RFID_TX_PIN  17
#endif

#define INA226_SHUNT_OHM  0.1f
// INA226 Adresse: 0x40 oder 0x41 (wird beim Start erkannt)
uint8_t INA226_ADDR = 0;
bool relayPresent = false;  // 4Relay (0x26) beim I2C-Scan gefunden
#define RFID_BAUD         115200  // YRM100-Modul: 115200 Baud (bestaetigt)
#define RELAY_I2C_ADDR    0x26   // 4Relay Modul
#define RELAY_REG         0x11   // Relais + LED (Bits 0–3 Relais)

// ============================================
// CONSTANTS
// ============================================
const unsigned long STATUS_SEND_INTERVAL_MS = 200;
const unsigned long INA_READ_INTERVAL_MS   = 4000;   // nur alle 4 s – Bus-Pausen gegen Error 263
const unsigned long RELAY_READ_INTERVAL_MS  = 4000;
const unsigned long I2C_BUS_PAUSE_MS       = 150;    // Pause vor/nach I2C, damit Display-Bus frei
const unsigned long RFID_TIMEOUT_MS        = 5000;
const size_t        ESPNOW_PAYLOAD_MAX     = 250;

void handleCommandFromTab(const String &jsonStr);

// Tab5-MAC (ESP32-C6): hier eintragen oder 0 = Broadcast
// Nach erstem Start steht CoreS3-MAC im Serial – Tab5 sendet an diese MAC
uint8_t tab5Mac[6] = {0, 0, 0, 0, 0, 0};
bool useBroadcast = true;  // true = an alle senden; Tab5 filtert

// ============================================
// GLOBAL VARIABLES
// ============================================
HardwareSerial rfidSerial(1);

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

float cachedVoltage = 0.0f;
float cachedCurrent = 0.0f;

// #region agent log H9: UART scan result storage
struct UartScanResult { int rx; int tx; uint32_t baud; int bytesPassive; int bytesCmd; };
UartScanResult uartScanResults[12];
int uartScanCount = 0;
int uartBestRx = 13, uartBestTx = 14;
uint32_t uartBestBaud = 9600;
bool uartScanFound = false;
// #endregion

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
  // #region agent log H2: raw INA226 register values (corrected formula: no >>3 shift)
  static uint32_t lastInaDbgMs = 0;
  if ((millis() - lastInaDbgMs) > 8000) {
    lastInaDbgMs = millis();
    Serial.printf("DBG H2: INA226 addr=0x%02X shuntRaw=%d busRaw=%u vCalc=%.4f\n",
      addr, (int)shuntRaw, (unsigned)busRaw, busRaw * 1.25e-3f);
  }
  // #endregion
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
  i2cBus->beginTransmission(RELAY_I2C_ADDR);
  i2cBus->write(RELAY_REG);
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
// ESP-NOW
// ============================================
void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len <= 0 || len > (int)ESPNOW_PAYLOAD_MAX) return;
  char buf[ESPNOW_PAYLOAD_MAX + 1];
  memcpy(buf, data, (size_t)len);
  buf[len] = '\0';
  handleCommandFromTab(String(buf));
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  Serial.println("ESP-NOW OK (direkt, kein WLAN)");
  Serial.print("CoreS3 MAC: ");
  Serial.println(WiFi.macAddress());
}

void sendStatusEspNow() {
  JsonDocument doc;
  doc["u"] = cachedVoltage;
  doc["i"] = cachedCurrent;
  doc["rfid"] = lastTag;
  JsonArray tagArray = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < rfidTagCount; i++)
    tagArray.add(rfidTagList[i]);
  doc["err"] = systemError;
  JsonArray relayArray = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 4; i++)
    relayArray.add(relayState[i] ? 1 : 0);
  doc["beep"] = beepFlag;

  String out;
  serializeJson(doc, out);
  if (out.length() >= ESPNOW_PAYLOAD_MAX) return;

  uint8_t raw[ESPNOW_PAYLOAD_MAX];
  size_t n = out.length();
  memcpy(raw, out.c_str(), n);

  if (useBroadcast) {
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_err_t r = esp_now_send(broadcast, raw, n);
    (void)r;
  } else {
    esp_now_send(tab5Mac, raw, n);
  }
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
  else if (strcmp(cmd, "rfid_play") == 0) {
    const char* tag = doc["tag"];
    if (tag && lastTag == String(tag)) {
      beepFlag = 1;
      Serial.printf("RFID matched: %s\n", tag);
    }
  }
}

// ============================================
// RFID – YRM100 Binaerprotokoll
// ============================================
// Single-Poll-Befehl: BB 00 22 00 00 22 7E
static const uint8_t YRM100_POLL[] = {0xBB,0x00,0x22,0x00,0x00,0x22,0x7E};

// Sendet Poll, liest Antwort-Frame, gibt EPC-String zurueck oder ""
String pollYRM100() {
  while (rfidSerial.available()) rfidSerial.read();  // Puffer leeren
  rfidSerial.write(YRM100_POLL, sizeof(YRM100_POLL));

  // Auf Antwort warten (max 250 ms)
  uint32_t t0 = millis();
  while (rfidSerial.available() < 8 && (millis() - t0) < 250) delay(2);
  if (rfidSerial.available() < 8) return "";

  // Frame komplett einlesen (max 32 Bytes, bis 7E oder Timeout)
  uint8_t buf[32]; int got = 0;
  t0 = millis();
  while (got < 32 && (millis() - t0) < 80) {
    if (rfidSerial.available()) {
      buf[got++] = rfidSerial.read();
      if (buf[got-1] == 0x7E && got >= 8) break;
    }
  }

  // #region agent log H9: raw poll response bytes
  static uint32_t lastPollDbgMs = 0;
  if (millis() - lastPollDbgMs > 2000) {
    lastPollDbgMs = millis();
    Serial.printf("DBG POLL got=%d:", got);
    for (int di = 0; di < got && di < 10; di++) Serial.printf(" %02X", buf[di]);
    Serial.println();
  }
  // #endregion

  // Pruefen: BB 01 22 = erfolgreiche Tag-Antwort
  if (got < 8 || buf[0] != 0xBB || buf[1] != 0x01) return "";
  if (buf[2] != 0x22) return "";  // 0xFF = kein Tag, andere = unbekannt

  // EPC extrahieren: Frame = BB 01 22 LH LL RSSI PC_H PC_L [EPC 12B] CRC_H CRC_L CS 7E
  // EPC beginnt bei Offset 8 (nach BB 01 22 LH LL RSSI PC_H PC_L)
  int epcStart = 8;
  int epcLen   = 12;  // 96-bit EPC
  if (got < epcStart + epcLen) return "";

  String epc = "";
  for (int i = epcStart; i < epcStart + epcLen; i++) {
    char h[3]; snprintf(h, 3, "%02X", buf[i]);
    epc += h;
  }
  return epc;
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
  // #region agent log H1: I2C bus init
  Serial.printf("DBG H1: I2C Wire1 SDA=%d SCL=%d (100kHz)\n", I2C_SDA, I2C_SCL);
  // #endregion
#else
  i2cBus->begin(I2C_SDA, I2C_SCL);
  i2cBus->setClock(100000);
  Serial.printf("DBG H1: I2C Wire0 SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
#endif
  Serial.println("I2C-Scan 0x08..0x77:");
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    i2cBus->beginTransmission(a);
    if (i2cBus->endTransmission() == 0) {
      Serial.printf("  0x%02X\n", a);
      if (a == RELAY_I2C_ADDR)
        relayPresent = true;
      if (a == 0x41) INA226_ADDR = 0x41;
      else if (a == 0x40 && INA226_ADDR == 0) INA226_ADDR = 0x40;
    }
  }
#if USE_WIRE1_FOR_I2C
  if (INA226_ADDR == 0 && !relayPresent) {
    Serial.println("Wire1 (G2/G1): keine Geraete – Fallback intern G12/G11.");
    i2cBus->end();
    delay(100);
    i2cBus = &Wire;
    i2cBus->begin(12, 11);
    i2cBus->setClock(100000);
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      i2cBus->beginTransmission(a);
      if (i2cBus->endTransmission() == 0) {
        Serial.printf("  [G12/11] 0x%02X\n", a);
        if (a == RELAY_I2C_ADDR) relayPresent = true;
        if (a == 0x41) INA226_ADDR = 0x41;
        else if (a == 0x40 && INA226_ADDR == 0) INA226_ADDR = 0x40;
      }
    }
  }
#endif
  if (INA226_ADDR != 0) {
    Serial.printf("INA226 Start: 0x%02X\n", INA226_ADDR);
    initINA226();
    delay(300);
    float v, i;
    if (readINA226At(0x41, v, i)) Serial.printf("INA226 0x41: %.2f V, %.3f A\n", v, i);
    if (readINA226At(0x40, v, i)) Serial.printf("INA226 0x40: %.2f V, %.3f A\n", v, i);
  } else
    Serial.println("INA226: weder 0x40 noch 0x41 am Bus.");
  if (relayPresent)
    Serial.println("4Relay (0x26) gefunden.");
  else
    Serial.println("4Relay (0x26) nicht am Bus – Relais-Lesen uebersprungen.");
  Serial.println("I2C (Port A) OK");

  // #region agent log H6-H9: UART-Scanner mit YRM100-Init-Befehl (H9: Modul braucht Befehl)
  {
    struct UartCfg { int rx; int tx; uint32_t baud; };
    const UartCfg candidates[] = {
      {13, 14,  9600}, {13, 14, 115200},
      {14, 13,  9600}, {14, 13, 115200},
      {16, 17,  9600}, {16, 17, 115200},
      {17, 16,  9600}, {17, 16, 115200},
      {18, 17,  9600}, {18, 17, 115200},  // H10: Standard CoreS3 Port C
      {17, 18,  9600}, {17, 18, 115200},  // H10: Standard CoreS3 Port C swapped
    };
    // YRM100 Single-Poll: BB 00 22 00 00 22 7E
    const uint8_t yrm100poll[] = {0xBB,0x00,0x22,0x00,0x00,0x22,0x7E};
    Serial.println("DBG H6-H9: UART-Scan (passiv 300ms + YRM100-Befehl 300ms je Kombo)");
    Serial.println(">>> RFID-Tag an das Lesegeraet halten! <<<");
    uartScanCount = 0;
    for (auto& c : candidates) {
      rfidSerial.end(); delay(30);
      rfidSerial.begin(c.baud, SERIAL_8N1, c.rx, c.tx);
      delay(300);
      int nPassive = rfidSerial.available();
      // YRM100 init senden (H9)
      rfidSerial.write(yrm100poll, sizeof(yrm100poll));
      delay(300);
      int nCmd = rfidSerial.available();
      Serial.printf("  RX=%d TX=%d @%u: passiv=%d cmd=%d\n",
        c.rx, c.tx, c.baud, nPassive, nCmd);
      if (uartScanCount < 12) {
        uartScanResults[uartScanCount++] = {c.rx, c.tx, c.baud, nPassive, nCmd};
      }
      if ((nPassive > 0 || nCmd > 0) && !uartScanFound) {
        uartScanFound = true;
        uartBestRx = c.rx; uartBestTx = c.tx; uartBestBaud = c.baud;
        Serial.printf("DBG MATCH: RX=%d TX=%d @%u (passiv=%d cmd=%d)\n",
          c.rx, c.tx, c.baud, nPassive, nCmd);
      }
    }
    rfidSerial.end(); delay(30);
    rfidSerial.begin(uartBestBaud, SERIAL_8N1, uartBestRx, uartBestTx);
    Serial.printf("DBG H6-H9: Final RX=%d TX=%d @%u Bd found=%d\n",
      uartBestRx, uartBestTx, uartBestBaud, (int)uartScanFound);
  }
  // #endregion

  if (relayPresent) {
    setRelay(0, false);
    setRelay(1, false);
    setRelay(2, false);
    setRelay(3, false);
    Serial.println("4Relay OK");
  }

  setupEspNow();

  M5.Display.fillScreen(TFT_BLACK);
  Serial.println("Setup complete");
}

// ============================================
// LOOP
// ============================================
void loop() {
  unsigned long now = millis();

  // RFID: YRM100 Poll alle 300 ms
  static uint32_t lastRfidPollMs = 0;
  if (now - lastRfidPollMs >= 300) {
    lastRfidPollMs = now;
    String epc = pollYRM100();
    // #region agent log H9: RFID poll result
    if (epc.length() > 0) {
      lastTag = epc;
      lastTagRefresh = now;
      rfidTagCount++;
      Serial.printf("RFID Tag: %s\n", epc.c_str());
    } else {
      static uint32_t lastRfidMissMs = 0;
      if (now - lastRfidMissMs > 5000) {
        lastRfidMissMs = now;
        Serial.printf("DBG H9: RFID poll – kein Tag (RX=%d TX=%d @%u)\n",
          uartBestRx, uartBestTx, uartBestBaud);
      }
    }
    // #endregion
  }

  if (now - lastTagRefresh > RFID_TIMEOUT_MS && lastTag != "-") {
    lastTag = "-";
    Serial.println("RFID timeout");
  }

  // #region agent log H6-H10: Scan-Ergebnisse alle 60s wiederholen
  static uint32_t lastScanPrintMs = 0;
  if (now > 15000 && (now - lastScanPrintMs) > 60000) {
    lastScanPrintMs = now;
    Serial.println("=== UART-SCAN ERGEBNISSE ===");
    for (int si = 0; si < uartScanCount; si++) {
      auto& r = uartScanResults[si];
      Serial.printf("  RX=%d TX=%d @%u: passiv=%d cmd=%d\n",
        r.rx, r.tx, r.baud, r.bytesPassive, r.bytesCmd);
    }
    Serial.printf("  => Aktiv: RX=%d TX=%d @%u found=%d\n",
      uartBestRx, uartBestTx, uartBestBaud, (int)uartScanFound);
    Serial.println("============================");
  }
  // #endregion

  // I2C nur in einem Block mit Pausen – reduziert Error 263 (Bus-Konflikt mit Display)
  if (now - lastInaReadMs >= INA_READ_INTERVAL_MS) {
    lastInaReadMs = now;
    lastRelayReadMs = now;
    delay(I2C_BUS_PAUSE_MS);
    readINA226();
    if (relayPresent) readRelayState();
    delay(I2C_BUS_PAUSE_MS);
  }

  if (now - lastStatusSendMs >= STATUS_SEND_INTERVAL_MS) {
    lastStatusSendMs = now;
    sendStatusEspNow();
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
    M5.Display.println("(ESP-NOW)");
  }

  M5.update();
  delay(10);
}

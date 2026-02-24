// M5StampPLC RFID Controller – PlatformIO (Build/Upload in Cursor)
// StamPLC mit UHF-RFID, INA226, 4x Relais, RS485 zum M5Tab
// Display: M5StamPLC + M5Unified + M5GFX (via platformio.ini lib_deps)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

#if __has_include(<M5StamPLC.h>)
  #include <M5StamPLC.h>
  #define USE_M5STAMPLC 1
#endif

#if USE_M5STAMPLC
  #define USE_DISPLAY 1
  #define M5_DISPLAY M5StamPLC.Display
#else
  #include <Wire.h>
  #include <M5Unified.h>
  #define USE_DISPLAY 1
  #define M5_DISPLAY M5.Display
#endif

// ============================================
// PIN CONFIGURATION
// ============================================
#if USE_M5STAMPLC
  // Offizielles StamPLC-Board: PWR-485 (laut pin_config.h)
  #define RS485_TX_PIN    0
  #define RS485_RX_PIN    39
  #define RS485_DIR_PIN   46
#else
  #define RS485_TX_PIN    17
  #define RS485_RX_PIN    16
  #define RS485_DIR_PIN   4
#endif

#define RFID_RX_PIN     26
#define RFID_TX_PIN     25

#if !USE_M5STAMPLC
  #define RELAY1_PIN  12
  #define RELAY2_PIN  13
  #define RELAY3_PIN  14
  #define RELAY4_PIN  15
  #define INA226_ADDR 0x41
  #define INA226_SHUNT_OHM 0.1f
#endif

// ============================================
// CONSTANTS
// ============================================
const unsigned long STATUS_SEND_INTERVAL_MS = 200;
const unsigned long INA_READ_INTERVAL_MS = 100;
const unsigned long RFID_TIMEOUT_MS = 5000;

// ============================================
// GLOBAL VARIABLES
// ============================================
HardwareSerial rs485Serial(2);
HardwareSerial rfidSerial(1);

String rfidBuffer = "";
String lastTag = "-";
unsigned long lastTagRefresh = 0;
bool relayState[4] = {false, false, false, false};
bool systemError = false;
unsigned long lastStatusSendMs = 0;
unsigned long lastInaReadMs = 0;
int beepFlag = 0;

#if USE_DISPLAY
unsigned long lastDisplayUpdateMs = 0;
#endif

String rfidTagList[20];
uint8_t rfidTagCount = 0;

float cachedVoltage = 0.0f;
float cachedCurrent = 0.0f;

// ============================================
// I2C / INA226 (nur ohne M5StamPLC – sonst API)
// ============================================
#if !USE_M5STAMPLC
int16_t readI2CReg16(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(addr, (uint8_t)2, (uint8_t)true) != 2) return 0;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  return (int16_t)((hi << 8) | lo);
}

uint16_t readI2CRegU16(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(addr, (uint8_t)2, (uint8_t)true) != 2) return 0;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  return (uint16_t)((hi << 8) | lo);
}
#endif

void readINA226() {
#if USE_M5STAMPLC
  cachedVoltage = M5StamPLC.getPowerVoltage();
  cachedCurrent = M5StamPLC.getIoSocketOutputCurrent();
#else
  int16_t shuntRaw = readI2CReg16(INA226_ADDR, 0x01);
  uint16_t busRaw = readI2CRegU16(INA226_ADDR, 0x02);
  float vShunt = shuntRaw * 2.5e-6f;
  float vBus = (busRaw >> 3) * 1.25e-3f;
  cachedVoltage = vBus;
  cachedCurrent = vShunt / INA226_SHUNT_OHM;
#endif
}

// ============================================
// RS485
// ============================================

void sendRS485JSON(const String &jsonStr) {
  digitalWrite(RS485_DIR_PIN, HIGH);
  delayMicroseconds(10);
  rs485Serial.print(jsonStr);
  rs485Serial.print("\n");
  rs485Serial.flush();
  delay(2 + (jsonStr.length() / 50));
  digitalWrite(RS485_DIR_PIN, LOW);
}

void setRelay(uint8_t idx, bool state) {
  if (idx >= 4) return;
  relayState[idx] = state;
#if USE_M5STAMPLC
  M5StamPLC.writePlcRelay(idx, state);
#else
  const uint8_t pins[] = {RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN};
  digitalWrite(pins[idx], state ? HIGH : LOW);
#endif
}

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

String readLineFromSerial(HardwareSerial &ser, String &buf, size_t maxLen = 256) {
  while (ser.available()) {
    int c = ser.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = buf;
      buf = "";
      return line;
    }
    buf += (char)c;
    if (buf.length() > maxLen)
      buf.remove(0, buf.length() - maxLen);
  }
  return String();
}

// ============================================
// SETUP
// ============================================

void setup() {
  // Serial sofort öffnen – dann siehst du im Monitor etwas, auch wenn Display-Init hängt
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\n=== M5StampPLC boot ===");

#if USE_DISPLAY
  Serial.println("Init Display...");
  #if USE_M5STAMPLC
  M5StamPLC.begin();
  #else
  {
    auto cfg = M5.config();
    cfg.clear_display = true;
    M5.begin(cfg);
  }
  #endif
  M5_DISPLAY.setRotation(1);
  M5_DISPLAY.setTextSize(2);
  M5_DISPLAY.fillScreen(TFT_BLACK);
  M5_DISPLAY.setTextColor(TFT_WHITE);
  M5_DISPLAY.setCursor(0, 0);
  M5_DISPLAY.println("M5StampPLC");
  M5_DISPLAY.println("RFID Controller");
  M5_DISPLAY.println("Starting...");
  delay(100);
  Serial.println("Display OK");
#endif

  Serial.println("=== M5StampPLC RFID Controller ===");
  #if USE_M5STAMPLC
  Serial.println("HW: M5StamPLC (Relays/INA226/RS485 via API)");
  #else
  Serial.println("HW: Generic ESP32 (GPIO/I2C)");
  #endif

#if !USE_M5STAMPLC
  Wire.begin(21, 22);
  Wire.setClock(400000);
  Serial.println("I2C initialized");
#endif

  pinMode(RS485_DIR_PIN, OUTPUT);
  digitalWrite(RS485_DIR_PIN, LOW);
  rs485Serial.begin(115200, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  Serial.println("RS485 initialized");

  rfidSerial.begin(115200, SERIAL_8N1, RFID_RX_PIN, RFID_TX_PIN);
  Serial.println("RFID UART initialized");

#if USE_M5STAMPLC
  M5StamPLC.writePlcAllRelay(0);
  Serial.println("Relays initialized (all OFF, via M5StamPLC)");
#else
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  for (int i = 0; i < 4; i++)
    setRelay((uint8_t)i, false);
  Serial.println("Relays initialized (all OFF)");
#endif

#if USE_DISPLAY
  M5_DISPLAY.fillScreen(TFT_BLACK);
#endif
  Serial.println("Setup complete");
}

// ============================================
// LOOP
// ============================================

void loop() {
  unsigned long now = millis();

  {
    String line = readLineFromSerial(rfidSerial, rfidBuffer);
    if (line.length() > 0) {
      line.trim();
      if (line.length() > 0) {
        lastTag = line;
        lastTagRefresh = now;
        Serial.printf("RFID Tag read: %s\n", lastTag.c_str());
      }
    }
  }

  {
    static String rs485Buf = "";
    String cmd = readLineFromSerial(rs485Serial, rs485Buf);
    if (cmd.length() > 0)
      handleCommandFromTab(cmd);
  }

  if (now - lastInaReadMs >= INA_READ_INTERVAL_MS) {
    lastInaReadMs = now;
    readINA226();
  }

#if USE_M5STAMPLC
  // Relais-Status von Hardware lesen (falls extern geändert)
  for (int i = 0; i < 4; i++)
    relayState[i] = M5StamPLC.readPlcRelay(i);
#endif

  if (now - lastTagRefresh > RFID_TIMEOUT_MS && lastTag != "-") {
    lastTag = "-";
    Serial.println("RFID timeout");
  }

  if (now - lastStatusSendMs >= STATUS_SEND_INTERVAL_MS) {
    lastStatusSendMs = now;
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
    sendRS485JSON(out);
    beepFlag = 0;
  }

#if USE_DISPLAY
  if (now - lastDisplayUpdateMs >= 500) {
    lastDisplayUpdateMs = now;
    M5_DISPLAY.fillScreen(TFT_BLACK);
    M5_DISPLAY.setTextSize(2);
    M5_DISPLAY.setTextColor(TFT_WHITE);
    M5_DISPLAY.setCursor(0, 0);
    M5_DISPLAY.printf("RFID: %s\n", lastTag.c_str());
    M5_DISPLAY.printf("U: %.2f V  I: %.3f A\n", cachedVoltage, cachedCurrent);
    M5_DISPLAY.print("R: ");
    for (int i = 0; i < 4; i++) M5_DISPLAY.print(relayState[i] ? "1" : "0");
    M5_DISPLAY.printf("  Tags:%d\n", rfidTagCount);
  }
  M5.update();
#endif

  delay(10);
}

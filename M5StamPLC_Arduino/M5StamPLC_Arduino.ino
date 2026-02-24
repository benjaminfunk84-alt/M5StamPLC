// M5StampPLC_RFIDController.ino
// StamPLC mit UHF-RFID (U107), INA226 (U200, I2C 0x41), 4x Relais, RS485 zum M5Tab
// Architektur: StamPLC liest RFID+INA226, sendet Sensor-Daten + Relais-Status via RS485 an M5Tab
// M5Tab sendet Relais-Befehle zurück (JSON)

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// Display: M5StamPLC-Bibliothek (board-spezifisch, initialisiert Display korrekt)
#if __has_include(<M5StamPLC.h>)
  #include <M5StamPLC.h>
  #define USE_DISPLAY 1
  #define DISPLAY M5StamPLC.Display
#else
  #include <M5Unified.h>
  #define USE_DISPLAY 1
  #define DISPLAY M5.Display
#endif

// ============================================
// PIN CONFIGURATION (M5StampPLC)
// ============================================
#define RS485_TX_PIN    17  // UART2 TX
#define RS485_RX_PIN    16  // UART2 RX
#define RS485_DIR_PIN    4  // Output Enable (DE/RE control)

#define RFID_RX_PIN     26  // UART1 RX (from UHF-RFID TX)
#define RFID_TX_PIN     25  // UART1 TX (to UHF-RFID RX)

#define RELAY1_PIN      12  // Relais 1: externe Umschaltung (Temperatursensor)
#define RELAY2_PIN      13  // Relais 2: Peripherie umschalten
#define RELAY3_PIN      14  // Relais 3: Peripherie Spannungslos (Not-Aus)
#define RELAY4_PIN      15  // Relais 4: zusätzlich

#define INA226_ADDR     0x41  // I2C Adresse (7-bit)
#define INA226_SHUNT_OHM 0.1f  // 0.1 Ohm Shunt

// I2C Standard: SDA=GPIO21, SCL=GPIO22 (M5StampPLC default)

// ============================================
// CONSTANTS
// ============================================
const unsigned long STATUS_SEND_INTERVAL_MS = 200;  // Sende Status alle 200ms
const unsigned long INA_READ_INTERVAL_MS = 100;     // Lese INA226 alle 100ms
const unsigned long RFID_TIMEOUT_MS = 5000;         // Tag-Timeout

// ============================================
// GLOBAL VARIABLES
// ============================================
HardwareSerial rs485Serial(2);   // UART2 for RS485
HardwareSerial rfidSerial(1);    // UART1 for UHF-RFID

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

// RFID-Speicher (max 20 Tags im RAM, nicht persistent)
String rfidTagList[20];
uint8_t rfidTagCount = 0;

// INA226-Messwerte (gecacht)
float cachedVoltage = 0.0f;
float cachedCurrent = 0.0f;

// ============================================
// I2C / INA226 FUNCTIONS
// ============================================

int16_t readI2CReg16(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;  // Fehler
  
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

void readINA226() {
  // Register 0x01: Shunt Voltage (2.5µV/LSB)
  int16_t shuntRaw = readI2CReg16(INA226_ADDR, 0x01);
  
  // Register 0x02: Bus Voltage (1.25mV/LSB, bits [15:3])
  uint16_t busRaw = readI2CRegU16(INA226_ADDR, 0x02);
  
  // Calculate
  float vShunt = shuntRaw * 2.5e-6f;  // 2.5 µV per LSB
  float vBus = (busRaw >> 3) * 1.25e-3f;  // 1.25 mV per LSB
  float current = vShunt / INA226_SHUNT_OHM;
  
  cachedVoltage = vBus;
  cachedCurrent = current;
}

// ============================================
// RS485 COMMUNICATION
// ============================================

void sendRS485JSON(const String &jsonStr) {
  // RS485 Sendeprozedur: DIR high → write → flush → DIR low
  digitalWrite(RS485_DIR_PIN, HIGH);
  delayMicroseconds(10);
  
  rs485Serial.print(jsonStr);
  rs485Serial.print("\n");
  rs485Serial.flush();
  
  delay(2 + (jsonStr.length() / 50));  // Kleine Verzögerung
  digitalWrite(RS485_DIR_PIN, LOW);
}

void handleCommandFromTab(const String &jsonStr) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.print("JSON Parse Error: ");
    Serial.println(err.f_str());
    return;
  }
  
  const char* cmd = doc["cmd"];
  if (!cmd) return;
  
  // --- set_relay: {"cmd":"set_relay", "idx":0, "val":1}
  if (strcmp(cmd, "set_relay") == 0) {
    int idx = doc["idx"] | -1;
    int val = doc["val"] | 0;
    
    if (idx >= 0 && idx < 4) {
      relayState[idx] = (val != 0);
      digitalWrite(RELAY1_PIN + idx, relayState[idx] ? HIGH : LOW);
      Serial.printf("Relay %d set to %d\n", idx, relayState[idx] ? 1 : 0);
    }
  }
  
  // --- rfid_learn: {"cmd":"rfid_learn"}
  else if (strcmp(cmd, "rfid_learn") == 0) {
    if (lastTag != "-" && lastTag.length() > 0 && rfidTagCount < 20) {
      // Prüfe Duplikat
      bool found = false;
      for (uint8_t i = 0; i < rfidTagCount; i++) {
        if (rfidTagList[i] == lastTag) {
          found = true;
          break;
        }
      }
      if (!found) {
        rfidTagList[rfidTagCount++] = lastTag;
        Serial.printf("RFID learned: %s (total: %d)\n", lastTag.c_str(), rfidTagCount);
        beepFlag = 1;
      }
    }
  }
  
  // --- rfid_play: {"cmd":"rfid_play", "tag":"..."}
  else if (strcmp(cmd, "rfid_play") == 0) {
    const char* tag = doc["tag"];
    if (tag && lastTag == String(tag)) {
      beepFlag = 1;
      // Optional: Aktion basierend auf Tag triggern
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
    } else {
      buf += (char)c;
      if (buf.length() > maxLen) {
        buf.remove(0, buf.length() - maxLen);
      }
    }
  }
  return String();
}

// ============================================
// SETUP
// ============================================

void setup() {
  // Display: M5StamPLC.begin() oder M5.begin() – ZUERST, vor allem anderen
  #if USE_DISPLAY
  #if __has_include(<M5StamPLC.h>)
  M5StamPLC.begin();
  #else
  {
    auto cfg = M5.config();
    cfg.clear_display = true;
    M5.begin(cfg);
  }
  #endif
  DISPLAY.setRotation(1);
  DISPLAY.setTextSize(2);
  DISPLAY.fillScreen(TFT_BLACK);
  DISPLAY.setTextColor(TFT_WHITE);
  DISPLAY.setCursor(0, 0);
  DISPLAY.println("M5StampPLC");
  DISPLAY.println("RFID Controller");
  DISPLAY.println("Starting...");
  delay(100);
  #endif

  // Serial Debug (GPIO1/GPIO3, 115200)
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== M5StampPLC RFID Controller ===");
  #if __has_include(<M5StamPLC.h>)
  Serial.println("Display: M5StamPLC aktiv");
  #else
  Serial.println("Display: M5Unified aktiv");
  #endif
  
  // I2C (SDA=GPIO21, SCL=GPIO22) – bei vollem M5StamPLC-Board evtl. 13/15
  Wire.begin(21, 22);
  Wire.setClock(400000);  // 400 kHz
  Serial.println("I2C initialized");
  
  // RS485 (UART2)
  pinMode(RS485_DIR_PIN, OUTPUT);
  digitalWrite(RS485_DIR_PIN, LOW);
  rs485Serial.begin(115200, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  Serial.println("RS485 initialized");
  
  // RFID UART (UART1)
  rfidSerial.begin(115200, SERIAL_8N1, RFID_RX_PIN, RFID_TX_PIN);
  Serial.println("RFID UART initialized");
  
  // Relais GPIO
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  for (int i = 0; i < 4; i++) {
    digitalWrite(RELAY1_PIN + i, LOW);  // All off
  }
  Serial.println("Relays initialized (all OFF)");
  
  #if USE_DISPLAY
  DISPLAY.fillScreen(TFT_BLACK);
  #endif
  
  Serial.println("Setup complete");
}

// ============================================
// MAIN LOOP
// ============================================

void loop() {
  unsigned long now = millis();
  
  // --- 1) RFID UART: Read incoming tags ---
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
  
  // --- 2) RS485 UART: Read incoming commands from M5Tab ---
  {
    static String rs485Buf = "";
    String cmd = readLineFromSerial(rs485Serial, rs485Buf);
    if (cmd.length() > 0) {
      handleCommandFromTab(cmd);
    }
  }
  
  // --- 3) INA226: Read periodically ---
  if (now - lastInaReadMs >= INA_READ_INTERVAL_MS) {
    lastInaReadMs = now;
    readINA226();
    // Optional: Serial.printf("INA226: %.2f V, %.3f A\n", cachedVoltage, cachedCurrent);
  }
  
  // --- 4) Timeout lastTag if no refresh ---
  if (now - lastTagRefresh > RFID_TIMEOUT_MS && lastTag != "-") {
    lastTag = "-";
    Serial.println("RFID timeout");
  }
  
  // --- 5) Send status JSON periodically ---
  if (now - lastStatusSendMs >= STATUS_SEND_INTERVAL_MS) {
    lastStatusSendMs = now;
    
    // Build JSON
    StaticJsonDocument<512> doc;
    doc["u"] = cachedVoltage;  // Bus voltage (V)
    doc["i"] = cachedCurrent;  // Current (A)
    doc["rfid"] = lastTag;
    
    JsonArray tagArray = doc.createNestedArray("list");
    for (uint8_t i = 0; i < rfidTagCount; i++) {
      tagArray.add(rfidTagList[i]);
    }
    
    doc["err"] = systemError;
    
    JsonArray relayArray = doc.createNestedArray("relays");
    for (int i = 0; i < 4; i++) {
      relayArray.add(relayState[i] ? 1 : 0);
    }
    
    doc["beep"] = beepFlag;
    
    String out;
    serializeJson(doc, out);
    sendRS485JSON(out);
    
    // Reset transient flags
    beepFlag = 0;
  }
  
  // --- 6) Display aktualisieren (ca. alle 500 ms) ---
  #if USE_DISPLAY
  if (now - lastDisplayUpdateMs >= 500) {
    lastDisplayUpdateMs = now;
    DISPLAY.fillScreen(TFT_BLACK);
    DISPLAY.setTextSize(2);
    DISPLAY.setTextColor(TFT_WHITE);
    DISPLAY.setCursor(0, 0);
    DISPLAY.printf("RFID: %s\n", lastTag.c_str());
    DISPLAY.printf("U: %.2f V  I: %.3f A\n", cachedVoltage, cachedCurrent);
    DISPLAY.print("R: ");
    for (int i = 0; i < 4; i++) DISPLAY.print(relayState[i] ? "1" : "0");
    DISPLAY.printf("  Tags:%d\n", rfidTagCount);
  }
  #endif
  
  #if USE_DISPLAY
  M5.update();  // M5-Geraete-Update (Buttons, Display etc.)
  #endif
  
  delay(10);
}

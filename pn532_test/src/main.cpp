/**
 * PN532 Modul-Test – nur CoreS3 + PN532 an Port A (I2C).
 * Kein M5/Display/WiFi, nur Serial + Wire1 + Adafruit_PN532.
 *
 * Verkabelung: PN532 VCC/GND/SDA/SCL an denselben Port wie in der Haupt-Firmware (Wire1 SDA=21, SCL=22).
 * DIP/Schalter am PN532 auf I2C stellen.
 *
 * Ablauf:
 * 1) I2C-Scan → muss 0x24 zeigen
 * 2) PN532 begin() + SAMConfig()
 * 3) TgInitAsTarget mit Test-UID "A1B2C3D4" (Emulation wie MIFARE Classic)
 * 4) Loop: getDataTarget/setDataTarget alle 100 ms; bei Daten vom Reader: Ausgabe
 */

#include <Arduino.h>
#include <Wire.h>
#include <M5Unified.h>
#include <Adafruit_PN532.h>

#define I2C_SDA  21  // wie Haupt-Firmware (nach M5.begin() freigegeben)
#define I2C_SCL  22
#define PN532_IRQ 4
#define PN532_RST 5

Adafruit_PN532 pn532(PN532_IRQ, PN532_RST, &Wire1);

static bool gPn532Ready = false;

// TgInitAsTarget für ISO 14443-A, MIFARE Classic 4-Byte-UID
// Variante A: Voller Parameterblock (wie in Haupt-Firmware genutzt)
// Variante B: Stark vereinfachter Block, falls A von dieser PN532-Firmware abgelehnt wird.
static bool initTargetMode(const uint8_t uid4[4]) {
  Serial.println("TgInitAsTarget: Variante A (voll) senden ...");
  uint8_t targetA[] = {
    0x8C, 0x00,                    // Command, Mode 14443A
    0x04, 0x00,                    // SENS_RES (ATQA) MIFARE Classic 4-byte UID
    uid4[0], uid4[1], uid4[2],     // NFCID1t (3 Byte; BCC ergänzt PN532)
    0x08,                          // SEL_RES (SAK) MIFARE Classic 1k
    0x01, 0xfe,                    // FeliCa POL_RES (NFCID2t Start)
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xc0,
    0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xff, 0xff,
    0xaa, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44,
    0x33, 0x22, 0x11, 0x01, 0x00,
    0x0d, 0x52, 0x46, 0x49, 0x44, 0x49, 0x4f,
    0x74, 0x20, 0x50, 0x4e, 0x35, 0x33, 0x32
  };
  if (pn532.sendCommandCheckAck(targetA, sizeof(targetA))) {
    Serial.println("TgInitAsTarget: Variante A OK");
    return true;
  }
  Serial.println("TgInitAsTarget: Variante A fehlgeschlagen, versuche Variante B (minimal) ...");

  // Variante B: Minimaler 14443A-Target-Block laut PN532-Doku
  uint8_t targetB[] = {
    0x8C, 0x00,                    // Command, Mode 14443A
    0x04, 0x00,                    // SENS_RES (ATQA)
    uid4[0], uid4[1], uid4[2],     // NFCID1t (3 Byte)
    0x08,                          // SEL_RES (SAK)
    0x00                           // Rest-Parameter = 0 (keine FeliCa / NFC-Dep)
  };
  if (pn532.sendCommandCheckAck(targetB, sizeof(targetB))) {
    Serial.println("TgInitAsTarget: Variante B OK");
    return true;
  }
  Serial.println("TgInitAsTarget: Variante B ebenfalls fehlgeschlagen.");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== PN532 Modul-Test (nur I2C Wire1) ===\n");

  M5.begin();
  delay(300);

  // M5Unified may have claimed Wire1 on pins 38/39; end() first to reassign to Port A (21/22).
  Wire1.end();
  delay(10);
  Wire1.begin(I2C_SDA, I2C_SCL);
  Wire1.setClock(100000);
  Wire1.setTimeOut(1000);

  Serial.printf("I2C Wire1 SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  Serial.println("I2C-Scan 0x08..0x77:");
  int found = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire1.beginTransmission(a);
    if (Wire1.endTransmission() == 0) {
      Serial.printf("  gefunden: 0x%02X\n", a);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  (keine Adresse gefunden – Verkabelung/Port A pruefen)\n");
    Serial.println("ENDE: Kein I2C-Geraet. PN532 an Wire1 (SDA=21, SCL=22) anschliessen.");
    return;
  }
  Serial.printf("Gesamt: %d Adresse(n)\n\n", found);

  if (!pn532.begin()) {
    Serial.println("FEHLER: PN532 begin() fehlgeschlagen (Adresse 0x24?).");
    Serial.println("ENDE.");
    return;
  }
  Serial.println("PN532 begin() OK");

  pn532.SAMConfig();
  Serial.println("PN532 SAMConfig() OK");

  uint8_t testUid[] = { 0xA1, 0xB2, 0xC3, 0xD4 };
  if (!initTargetMode(testUid)) {
    Serial.println("FEHLER: TgInitAsTarget fehlgeschlagen.");
    Serial.println("ENDE.");
    return;
  }
  Serial.println("PN532 TgInitAsTarget OK – UID A1B2C3D4");
  Serial.println("\n>>> Reader (CHARX/Handy) an PN532 halten – Daten werden unten ausgegeben.\n");
  gPn532Ready = true;
}

void loop() {
  if (!gPn532Ready) {
    delay(1000);
    return;
  }
  uint8_t cmd[64];
  uint8_t cmdLen;
  if (!pn532.getDataTarget(cmd, &cmdLen)) {
    delay(100);
    return;
  }
  Serial.printf("[Reader-Daten %u Bytes]: ", (unsigned)cmdLen);
  for (uint8_t i = 0; i < cmdLen && i < 32; i++)
    Serial.printf("%02X ", cmd[i]);
  if (cmdLen > 32) Serial.print("...");
  Serial.println();

  pn532.setDataTarget(cmd, cmdLen);
  delay(50);
}

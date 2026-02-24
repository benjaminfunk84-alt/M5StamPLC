/**
 * I2C-Scan für M5CoreS3
 * Scant beide möglichen I2C-Busse und gibt alle gefundenen Adressen aus.
 *
 * Bus 1: Port A (G2=SDA, G1=SCL) – M-Bus Pin 19/20
 * Bus 2: Intern (G12=SDA, G11=SCL) – M-Bus Pin 17/18, oft für CatM Port A
 *
 * Nutzung: pio run -t upload && pio device monitor -b 115200
 */

#include <Arduino.h>
#include <Wire.h>

// Port A am CoreS3 / CatM (M-Bus Pin 19–20)
#define PORT_A_SDA  2
#define PORT_A_SCL  1
// Interner I2C (M-Bus Pin 17–18) – Display, Kamera, evtl. CatM Port A
#define INTERNAL_SDA  12
#define INTERNAL_SCL  11

static int scanBus(int sda, int scl, const char* label) {
  Wire.end();
  delay(20);
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  delay(50);

  Serial.printf("\n--- %s (SDA=%d, SCL=%d) ---\n", label, sda, scl);
  int count = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("  0x%02X", addr);
      if (addr == 0x26) Serial.print(" (4Relay)");
      else if (addr >= 0x40 && addr <= 0x43) Serial.print(" (z.B. INA226)");
      else if (addr == 0x34) Serial.print(" (AXP2101)");
      else if (addr == 0x51) Serial.print(" (BM8563 RTC)");
      else if (addr == 0x58) Serial.print(" (AW9523)");
      else if (addr == 0x69) Serial.print(" (BMI270)");
      else if (addr == 0x38) Serial.print(" (FT6336 Touch)");
      else if (addr == 0x21) Serial.print(" (GC0308 Cam)");
      else if (addr == 0x23) Serial.print(" (LTR553)");
      else if (addr == 0x36) Serial.print(" (AW88298)");
      else if (addr == 0x10) Serial.print(" (BMM150)");
      Serial.println();
      count++;
    }
    delay(2);
  }
  Serial.printf("  => %d Adresse(n)\n", count);
  return count;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n=== I2C-Scan M5CoreS3 (beide Busse) ===\n");
}

void loop() {
  int a = scanBus(PORT_A_SDA, PORT_A_SCL, "Port A (G2/G1)");
  int b = scanBus(INTERNAL_SDA, INTERNAL_SCL, "Intern (G12/G11)");

  Serial.println("\n--- Zusammenfassung ---");
  Serial.printf("  Port A (G2/G1):   %d Geraete\n", a);
  Serial.printf("  Intern (G12/G11): %d Geraete\n", b);
  if (a == 0 && b > 0)
    Serial.println("\n  -> INA226/4Relay haengen vermutlich am internen Bus (G12/G11).");
  Serial.println("\n  Naechster Scan in 5 s ...\n");
  delay(5000);
}

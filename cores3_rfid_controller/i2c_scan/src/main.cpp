/**
 * I2C-Scan für M5Stack CoreS3 – nur Port A (Wire1, GPIO 21/22).
 *
 * Port A = Grove Port A (rot), SDA=21, SCL=22, 5V, GND – PN532, INA226, 4Relay.
 * Wire (12/11) wird nicht angefassen: Jeder Aufruf von Wire.begin(12,11) macht
 * danach Wire1 kaputt (Invalid pin: 22). Daher nur Wire1.
 */

#include <Arduino.h>
#include <Wire.h>
#include <M5Unified.h>

#define PORT_A_SDA  21
#define PORT_A_SCL  22

static int scanPortA() {
  Wire1.begin(PORT_A_SDA, PORT_A_SCL);
  Wire1.setClock(100000);
  Wire1.setTimeOut(1000);
  delay(50);

  Serial.printf("\n--- Port A (Wire1, SDA=%d SCL=%d) ---\n", PORT_A_SDA, PORT_A_SCL);
  int count = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire1.beginTransmission(addr);
    uint8_t err = Wire1.endTransmission();
    if (err == 0) {
      Serial.printf("  0x%02X", addr);
      if (addr == 0x26) Serial.print(" (4Relay)");
      else if (addr == 0x24) Serial.print(" (PN532 oder RC522)");
      else if (addr >= 0x40 && addr <= 0x43) Serial.print(" (z.B. INA226)");
      else if (addr == 0x34) Serial.print(" (AXP2101)");
      else if (addr == 0x51) Serial.print(" (BM8563 RTC)");
      else if (addr == 0x58) Serial.print(" (AW9523)");
      else if (addr == 0x69) Serial.print(" (BMI270)");
      else if (addr == 0x38) Serial.print(" (FT6336 Touch)");
      else if (addr == 0x21) Serial.print(" (GC0308 Cam)");
      else if (addr == 0x23) Serial.print(" (LTR553/RC522)");
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
  delay(500);
  Serial.println("\n\n=== I2C-Scan M5CoreS3 (nur Port A, Wire1 21/22) ===\n");
  M5.begin();
  delay(300);
  Serial.println("Port A wird alle 5 s gescannt. Intern-Bus (Wire) wird nicht genutzt.\n");
}

void loop() {
  int a = scanPortA();

  Serial.println("\n--- Zusammenfassung ---");
  Serial.printf("  Port A (21/22): %d Geraete\n", a);
  if (a == 0)
    Serial.println("\n  -> Verkabelung (SDA=21, SCL=22, 5V, GND) und DIP (I2C) pruefen.");
  Serial.println("\n  Naechster Scan in 5 s ...\n");
  delay(5000);
}

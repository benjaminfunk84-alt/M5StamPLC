// M5Tab5 Controller – Touch-UI für Relay-Steuerung und RFID-Verwaltung
// Kommunikation mit CoreS3: UART2 (M5-Bus G38=RX, G37=TX) → ESP32-C6 Bridge → ESP-NOW
// Der C6 leitet JSON-Pakete von CoreS3 weiter und sendet Befehle zurück.

#include <Arduino.h>
#include <M5Unified.h>
#include <ArduinoJson.h>

// ============================================
// FARB-PALETTE (RGB565)
// ============================================
static const uint32_t C_BG       = 0x0C0F14;  // Sehr dunkel (Fast schwarz-blau)
static const uint32_t C_PANEL    = 0x141A21;  // Dunkelgrau-blau (Panel)
static const uint32_t C_CARD     = 0x1E2530;  // Karte inaktiv
static const uint32_t C_CARD_ON  = 0x0D2B2E;  // Karte aktiv (dunkles Cyan)
static const uint32_t C_DIVIDER  = 0x1E3040;  // Trennlinie
static const uint32_t C_CYAN     = 0x00D4FF;  // Haupt-Akzent
static const uint32_t C_GREEN    = 0x00E676;  // Grün (Relais EIN)
static const uint32_t C_RED      = 0xFF3D3D;  // Rot (Löschen)
static const uint32_t C_ORANGE   = 0xFFAA00;  // Orange (Scan-Modus)
static const uint32_t C_WHITE    = 0xFFFFFF;
static const uint32_t C_GREY     = 0x7A8899;
static const uint32_t C_DIMGREY  = 0x3A4555;
static const uint32_t C_BLACK    = 0x000000;

// ============================================
// LAYOUT
// ============================================
static int DW, DH;           // Display-Dimensionen (nach M5.begin())
static const int S_H  = 64;  // Statusleiste Höhe
static const int L_W  = 460; // Linkes Panel (Relais) Breite
// Rechtes Panel: DW - L_W (RFID)

// ============================================
// ZUSTAND
// ============================================
float   gVolt    = 0.0f, gAmp = 0.0f;
bool    gRelay[4]   = {};
String  gCurTag     = "-";
String  gTagList[20];
uint8_t gTagCount   = 0;
bool    gConnected  = false;
unsigned long gLastRxMs = 0;

// Scan-Modus (5s – neuen Tag einlesen)
bool          gScanMode   = false;
unsigned long gScanEndMs  = 0;

// Write-Modus (10s – Tag auf Karte schreiben)
bool          gWriteMode  = false;
int           gWriteIdx   = -1;
unsigned long gWriteEndMs = 0;

// Scroll für Tag-Liste
int gTagScroll = 0;

// Dirty-Flags für selektives Neu-Zeichnen
bool gDirtyStatus = true;
bool gDirtyRelay  = true;
bool gDirtyRfid   = true;

// ============================================
// SPRITES
// ============================================
LGFX_Sprite sprStatus(nullptr);
LGFX_Sprite sprRelay(nullptr);
LGFX_Sprite sprRfid(nullptr);

// ============================================
// HIT-RECTS (in Display-Koordinaten)
// ============================================
struct Rect { int x, y, w, h;
  bool hit(int tx, int ty) const {
    return tx >= x && tx < x+w && ty >= y && ty < y+h;
  }
};
Rect rcRelay[4];
Rect rcTagCard[20], rcTagDel[20], rcTagWrt[20];
Rect rcScanBtn;
Rect rcScrollUp, rcScrollDown;

// ============================================
// UART-Kommunikation (M5-Bus: G38=RX, G37=TX)
// ESP32-P4 liest/schreibt JSON-Zeilen zum C6-Bridge-Chip
// ============================================
static const int  UART_RX  = 38;
static const int  UART_TX  = 37;
static const int  UART_BAUD = 115200;
static const size_t PMAX   = 250;

// UART2 auf P4 (HardwareSerial-Instanz)
static HardwareSerial bridgeSerial(2);

static void sendCmd(const char* json) {
  bridgeSerial.println(json);
  Serial.print("TX> "); Serial.println(json);
}

// Forward decl
static void drawAll(bool full = false);
static void drawRfidPanel();
static void drawStatusBar();

static void processJsonLine(const char* buf) {
  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

  float v = doc["u"] | gVolt;
  float a = doc["i"] | gAmp;
  bool voltChanged = (fabsf(v - gVolt) > 0.01f || fabsf(a - gAmp) > 0.001f);
  gVolt = v; gAmp = a;

  const char* rtag = doc["rfid"] | "-";
  String tag = String(rtag);
  bool tagChanged = (tag != gCurTag);
  gCurTag = tag;

  JsonArrayConst rArr = doc["relays"].as<JsonArrayConst>();
  bool relayChanged = false;
  if (rArr.size() == 4) {
    for (int i = 0; i < 4; i++) {
      bool s = (rArr[i].as<int>() == 1);
      if (s != gRelay[i]) { gRelay[i] = s; relayChanged = true; }
    }
  }

  JsonArrayConst tArr = doc["list"].as<JsonArrayConst>();
  if (tArr.size() > 0) {
    uint8_t newCount = 0;
    String newList[20];
    for (JsonVariantConst t : tArr) {
      if (newCount < 20) newList[newCount++] = t.as<String>();
    }
    if (newCount != gTagCount) {
      gTagCount = newCount;
      for (int i = 0; i < (int)gTagCount; i++) gTagList[i] = newList[i];
      gDirtyRfid = true;
    }
  }

  bool wasConnected = gConnected;
  gConnected = true;
  gLastRxMs  = millis();

  // Scan-Modus: neuen Tag automatisch hinzufügen
  if (gScanMode && millis() < gScanEndMs && gCurTag.length() > 0 && gCurTag != "-") {
    bool found = false;
    for (int i = 0; i < (int)gTagCount; i++) { if (gTagList[i] == gCurTag) { found = true; break; } }
    if (!found && gTagCount < 20) {
      gTagList[gTagCount++] = gCurTag;
      sendCmd("{\"cmd\":\"rfid_learn\"}");
      gScanMode  = false;
      gDirtyRfid = true;
      gDirtyStatus = true;
    }
  }

  if (relayChanged)                               gDirtyRelay  = true;
  if (voltChanged || tagChanged || !wasConnected) gDirtyStatus = true;
  if (tagChanged)                                 gDirtyRfid   = true;
}

static void readBridgeSerial() {
  static char rxBuf[PMAX + 1];
  static int  rxIdx = 0;
  while (bridgeSerial.available()) {
    char c = (char)bridgeSerial.read();
    if (c == '\n' || c == '\r') {
      if (rxIdx > 0) {
        rxBuf[rxIdx] = '\0';
        processJsonLine(rxBuf);
        rxIdx = 0;
      }
    } else if (rxIdx < (int)PMAX) {
      rxBuf[rxIdx++] = c;
    }
  }
}

static void setupBridge() {
  bridgeSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX, UART_TX);
  Serial.printf("UART Bridge: RX=%d TX=%d @%d\n", UART_RX, UART_TX, UART_BAUD);
}

// ============================================
// ZEICHNEN – Status-Leiste
// ============================================
static void drawStatusBar() {
  int sw = DW, sh = S_H;
  sprStatus.fillRect(0, 0, sw, sh, (uint32_t)C_PANEL);
  sprStatus.drawFastHLine(0, sh - 1, sw, (uint32_t)C_DIVIDER);

  // Verbindungs-LED
  bool alive = (millis() - gLastRxMs) < 3500 && gLastRxMs > 0;
  uint32_t ledCol = alive ? C_GREEN : C_DIMGREY;
  sprStatus.fillCircle(22, sh / 2, 8, ledCol);

  // Titel
  sprStatus.setTextColor(C_CYAN, C_PANEL);
  sprStatus.setTextSize(2);
  sprStatus.setCursor(40, 18);
  sprStatus.print("CoreS3");

  sprStatus.setTextColor(C_GREY, C_PANEL);
  sprStatus.setTextSize(1);
  sprStatus.setCursor(125, 24);
  sprStatus.print(alive ? "ESP-NOW verbunden" : "warten auf Signal...");

  // Spannung / Strom
  char buf[32];
  sprStatus.setTextSize(2);
  sprStatus.setTextColor(C_WHITE, C_PANEL);
  sprStatus.setCursor(380, 18);
  snprintf(buf, sizeof(buf), "%.2f V", gVolt);
  sprStatus.print(buf);

  sprStatus.setTextColor(C_GREY, C_PANEL);
  sprStatus.setCursor(510, 18);
  snprintf(buf, sizeof(buf), "%.3f A", gAmp);
  sprStatus.print(buf);

  // Aktueller Tag
  sprStatus.setCursor(680, 18);
  sprStatus.setTextColor(C_DIMGREY, C_PANEL);
  sprStatus.print("TAG: ");
  sprStatus.setTextColor(gCurTag != "-" ? C_CYAN : C_GREY, C_PANEL);
  sprStatus.print(gCurTag.length() > 10 ? gCurTag.substring(0, 10) : gCurTag);

  sprStatus.pushSprite(&M5.Display, 0, 0);
  gDirtyStatus = false;
}

// ============================================
// ZEICHNEN – Relay-Panel
// ============================================
static void drawRelayPanel() {
  int pw = L_W;
  int ph = DH - S_H;
  sprRelay.fillRect(0, 0, pw, ph, (uint32_t)C_PANEL);

  // Panel-Titel
  sprRelay.setTextColor(C_GREY, C_PANEL);
  sprRelay.setTextSize(1);
  sprRelay.setCursor(16, 12);
  sprRelay.print("STEUERUNG");

  const int CARD_H = 125;
  const int CARD_W = pw - 24;
  const int CARD_X = 12;
  const int START_Y = 36;
  const int GAP    = 12;

  for (int i = 0; i < 4; i++) {
    int cy = START_Y + i * (CARD_H + GAP);
    bool on = gRelay[i];

    // Karte
    uint32_t bg  = on ? C_CARD_ON : C_CARD;
    uint32_t brd = on ? C_CYAN    : C_DIVIDER;
    sprRelay.fillRoundRect(CARD_X, cy, CARD_W, CARD_H, 12, bg);
    sprRelay.drawRoundRect(CARD_X, cy, CARD_W, CARD_H, 12, brd);

    // Linker Akzent-Balken
    if (on) sprRelay.fillRect(CARD_X, cy + 12, 4, CARD_H - 24, C_CYAN);

    // Relay-Label
    sprRelay.setTextColor(C_GREY, bg);
    sprRelay.setTextSize(1);
    sprRelay.setCursor(CARD_X + 18, cy + 12);
    sprRelay.printf("RELAIS %d", i + 1);

    // Großer Status-Text
    sprRelay.setTextSize(3);
    sprRelay.setTextColor(on ? C_GREEN : C_WHITE, bg);
    sprRelay.setCursor(CARD_X + 18, cy + 38);
    sprRelay.print(on ? "EIN" : "AUS");

    // Status-Indikator (LED Kreis)
    uint32_t ledC = on ? C_GREEN : C_DIMGREY;
    sprRelay.fillCircle(CARD_X + CARD_W - 30, cy + CARD_H / 2, 14, ledC);
    if (on) sprRelay.fillCircle(CARD_X + CARD_W - 30, cy + CARD_H / 2, 8, C_WHITE);

    // Tipp-Hinweis
    sprRelay.setTextSize(1);
    sprRelay.setTextColor(C_DIMGREY, bg);
    sprRelay.setCursor(CARD_X + 18, cy + CARD_H - 20);
    sprRelay.print("Tippen zum Umschalten");

    // Hit-Area in Display-Koordinaten
    rcRelay[i] = {CARD_X, S_H + cy, CARD_W, CARD_H};
  }

  sprRelay.pushSprite(&M5.Display, 0, S_H);
  gDirtyRelay = false;
}

// ============================================
// ZEICHNEN – RFID-Panel
// ============================================
static void drawRfidPanel() {
  int px = L_W + 1;
  int pw = DW - px;
  int ph = DH - S_H;
  int ow = pw;  // original width for sprite (zero-based)

  sprRfid.fillRect(0, 0, ow, ph, (uint32_t)C_PANEL);

  // Panel-Titel
  char hdr[32];
  snprintf(hdr, sizeof(hdr), "RFID TAGS  (%d/20)", gTagCount);
  sprRfid.setTextColor(C_GREY, C_PANEL);
  sprRfid.setTextSize(1);
  sprRfid.setCursor(16, 12);
  sprRfid.print(hdr);

  // ---- Banner: Scan-Modus / Write-Modus ----
  int bannerH = 0;
  unsigned long now = millis();
  if (gScanMode && now < gScanEndMs) {
    bannerH = 48;
    int rem = (int)((gScanEndMs - now) / 1000) + 1;
    sprRfid.fillRoundRect(12, 30, ow - 24, bannerH, 8, (uint32_t)C_ORANGE);
    sprRfid.setTextColor(C_BLACK, C_ORANGE);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(20, 46);
    sprRfid.printf("Tag an Leser halten... %ds", rem);
  } else if (gWriteMode && now < gWriteEndMs) {
    bannerH = 48;
    int rem = (int)((gWriteEndMs - now) / 1000) + 1;
    sprRfid.fillRoundRect(12, 30, ow - 24, bannerH, 8, (uint32_t)C_GREEN);
    sprRfid.setTextColor(C_BLACK, C_GREEN);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(20, 46);
    sprRfid.printf("Beschreibbare Karte halten... %ds", rem);
  }

  // ---- Tag-Liste ----
  const int SCAN_BTN_H = 58;
  const int TAG_H      = 72;
  const int TAG_X      = 12;
  const int TAG_W      = ow - 24;
  const int LIST_Y     = 34 + bannerH + 4;  // Start der Liste
  int listH    = ph - LIST_Y - SCAN_BTN_H - 16;
  int maxVis   = max(1, listH / (TAG_H + 8));

  // Scroll-Korrektur
  if (gTagScroll > (int)gTagCount - maxVis) gTagScroll = max(0, (int)gTagCount - maxVis);

  int visEnd = min((int)gTagCount, gTagScroll + maxVis);

  for (int i = gTagScroll; i < visEnd; i++) {
    int cy = LIST_Y + (i - gTagScroll) * (TAG_H + 8);
    bool isWriteTarget = (gWriteMode && gWriteIdx == i);
    uint32_t bg  = isWriteTarget ? 0x0D2B20 : C_CARD;
    uint32_t brd = isWriteTarget ? C_GREEN  : C_DIVIDER;

    sprRfid.fillRoundRect(TAG_X, cy, TAG_W, TAG_H, 8, bg);
    sprRfid.drawRoundRect(TAG_X, cy, TAG_W, TAG_H, 8, brd);

    // Index-Label
    sprRfid.setTextSize(1);
    sprRfid.setTextColor(C_GREY, bg);
    sprRfid.setCursor(TAG_X + 12, cy + 8);
    sprRfid.printf("TAG %02d", i + 1);

    // UID
    sprRfid.setTextSize(2);
    sprRfid.setTextColor(C_WHITE, bg);
    sprRfid.setCursor(TAG_X + 12, cy + 26);
    sprRfid.print(gTagList[i]);

    // Write-Button (cyan)
    int wBtnW = 80, wBtnH = 34;
    int wBtnX = TAG_W - wBtnW - wBtnH - 14;  // links vom Del-Button
    int wBtnY = cy + (TAG_H - wBtnH) / 2;
    uint32_t wCol = isWriteTarget ? C_GREEN : C_CYAN;
    sprRfid.fillRoundRect(TAG_X + wBtnX, wBtnY, wBtnW, wBtnH, 6, wCol);
    sprRfid.setTextColor(C_BLACK, wCol);
    sprRfid.setTextSize(1);
    sprRfid.setCursor(TAG_X + wBtnX + 10, wBtnY + 11);
    sprRfid.print(isWriteTarget ? "ABBRECH" : "SCHREIB");

    // Del-Button (rot)
    int dBtnW = wBtnH, dBtnH = wBtnH;
    int dBtnX = TAG_W - dBtnW - 2;
    int dBtnY = wBtnY;
    sprRfid.fillRoundRect(TAG_X + dBtnX, dBtnY, dBtnW, dBtnH, 6, (uint32_t)C_RED);
    sprRfid.setTextColor(C_WHITE, C_RED);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(TAG_X + dBtnX + 10, dBtnY + 8);
    sprRfid.print("X");

    // Hit-Areas in Display-Koordinaten
    rcTagCard[i] = {px + TAG_X,         S_H + cy, TAG_W - wBtnW - dBtnW - 20, TAG_H};
    rcTagWrt[i]  = {px + TAG_X + wBtnX, S_H + wBtnY, wBtnW, wBtnH};
    rcTagDel[i]  = {px + TAG_X + dBtnX, S_H + dBtnY, dBtnW, dBtnH};
  }

  // Keine Tags
  if (gTagCount == 0) {
    sprRfid.setTextColor(C_DIMGREY, C_PANEL);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(ow / 2 - 130, ph / 2 - 40);
    sprRfid.print("Keine Tags gespeichert.");
    sprRfid.setCursor(ow / 2 - 130, ph / 2 - 10);
    sprRfid.print("Unten auf + tippen.");
  }

  // Scroll-Buttons (wenn nötig)
  if (gTagCount > maxVis) {
    // Nach oben
    uint32_t upC = (gTagScroll > 0) ? C_CYAN : C_DIMGREY;
    sprRfid.fillTriangle(ow - 22, LIST_Y + 10, ow - 10, LIST_Y + 30, ow - 34, LIST_Y + 30, upC);
    rcScrollUp = {px + ow - 40, S_H + LIST_Y, 40, 40};
    // Nach unten
    int dnY = LIST_Y + listH - 30;
    uint32_t dnC = (visEnd < (int)gTagCount) ? C_CYAN : C_DIMGREY;
    sprRfid.fillTriangle(ow - 22, dnY + 20, ow - 10, dnY, ow - 34, dnY, dnC);
    rcScrollDown = {px + ow - 40, S_H + dnY - 10, 40, 40};
  } else {
    rcScrollUp = rcScrollDown = {0, 0, 0, 0};
  }

  // ---- Scan-Button ----
  int sbY = ph - SCAN_BTN_H - 4;
  bool scanActive = gScanMode && now < gScanEndMs;
  uint32_t sbCol  = scanActive ? C_ORANGE : C_CYAN;
  sprRfid.fillRoundRect(TAG_X, sbY, TAG_W, SCAN_BTN_H, 10, sbCol);
  sprRfid.setTextColor(C_BLACK, sbCol);
  sprRfid.setTextSize(2);
  const char* sbTxt = scanActive ? "Scannen aktiv... (Tippen = Stop)"
                                 : "+ Neuen Tag einlesen  (5 Sek.)";
  int tw = strlen(sbTxt) * 12;  // rough estimate
  sprRfid.setCursor(max(0, (ow - tw) / 2), sbY + 18);
  sprRfid.print(sbTxt);
  rcScanBtn = {px + TAG_X, S_H + sbY, TAG_W, SCAN_BTN_H};

  sprRfid.pushSprite(&M5.Display, px, S_H);
  gDirtyRfid = false;
}

// ============================================
// ZEICHNEN – Alles
// ============================================
static void drawAll(bool full) {
  if (full) {
    M5.Display.fillScreen(C_PANEL);
    // Trenn-Linie zwischen Relay- und RFID-Panel
    M5.Display.drawFastVLine(L_W, S_H, DH - S_H, C_DIVIDER);
    gDirtyStatus = gDirtyRelay = gDirtyRfid = true;
  }
  if (gDirtyStatus) drawStatusBar();
  if (gDirtyRelay)  drawRelayPanel();
  if (gDirtyRfid)   drawRfidPanel();
}

// ============================================
// TOUCH HANDLING
// ============================================
static void handleTouch(int tx, int ty) {
  // Scroll
  if (rcScrollUp.hit(tx, ty))   { gTagScroll = max(0, gTagScroll - 1); gDirtyRfid = true; return; }
  if (rcScrollDown.hit(tx, ty)) { gTagScroll++; gDirtyRfid = true; return; }

  // Scan-Button
  if (rcScanBtn.hit(tx, ty)) {
    gScanMode  = !gScanMode;
    gScanEndMs = gScanMode ? millis() + 5000 : 0;
    gDirtyRfid = true;
    return;
  }

  // Relay-Karten
  for (int i = 0; i < 4; i++) {
    if (rcRelay[i].hit(tx, ty)) {
      char cmd[64];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"set_relay\",\"idx\":%d,\"val\":%d}", i, gRelay[i] ? 0 : 1);
      sendCmd(cmd);
      gRelay[i]   = !gRelay[i];  // Optimistisches Update
      gDirtyRelay = true;
      return;
    }
  }

  // Tag-Buttons (nur sichtbare)
  int maxVis = max(1, (DH - S_H - 34 - 74) / 80);
  int visEnd = min((int)gTagCount, gTagScroll + maxVis);
  for (int i = gTagScroll; i < visEnd && i < 20; i++) {
    // Löschen
    if (rcTagDel[i].hit(tx, ty)) {
      char cmd[128];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"rfid_delete\",\"uid\":\"%s\"}", gTagList[i].c_str());
      sendCmd(cmd);
      for (int j = i; j < gTagCount - 1; j++) gTagList[j] = gTagList[j + 1];
      if (gTagCount > 0) gTagCount--;
      if (gTagScroll > 0 && gTagScroll >= (int)gTagCount) gTagScroll--;
      gWriteMode = false; gWriteIdx = -1;
      gDirtyRfid = true;
      return;
    }
    // Schreiben / Abbrechen
    if (rcTagWrt[i].hit(tx, ty)) {
      if (gWriteMode && gWriteIdx == i) {
        gWriteMode = false; gWriteIdx = -1;  // Abbrechen
      } else {
        gWriteMode  = true;
        gWriteIdx   = i;
        gWriteEndMs = millis() + 10000;
      }
      gDirtyRfid = true;
      return;
    }
  }
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== M5Tab5 RFID Controller ===");

  auto cfg = M5.config();
  cfg.clear_display = true;
  M5.begin(cfg);

  M5.Display.setRotation(1);   // Landscape 1280x720
  M5.Display.setBrightness(220);
  DW = M5.Display.width();
  DH = M5.Display.height();
  Serial.printf("Display: %d x %d\n", DW, DH);

  // Sprites erstellen (nutzen PSRAM auf Tab5)
  // Große Sprites (>300KB) müssen aus PSRAM allokiert werden (Tab5 hat 32MB PSRAM)
  // Free heap ~394KB reicht nicht für Relay(590KB) + Rfid(1MB)
  sprStatus.createSprite(DW, S_H);
  sprRelay.setPsram(true);
  sprRelay.createSprite(L_W, DH - S_H);
  sprRfid.setPsram(true);
  sprRfid.createSprite(DW - L_W - 1, DH - S_H);

  // Splash screen
  M5.Display.fillScreen(C_BG);
  M5.Display.setTextColor(C_CYAN);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(DW / 2 - 240, DH / 2 - 20);
  M5.Display.print("Verbinde mit CoreS3...");

  setupBridge();
  drawAll(true);
  Serial.println("Setup complete");
}

// ============================================
// LOOP
// ============================================
void loop() {
  M5.update();
  unsigned long now = millis();

  // UART Bridge lesen
  readBridgeSerial();

  // Touch-Eingabe
  if (M5.Touch.getCount() > 0) {
    auto td = M5.Touch.getDetail(0);
    if (td.wasPressed()) {
      handleTouch(td.x, td.y);
    }
  }

  // Verbindungs-Timeout
  static bool wasConn = false;
  bool isConn = (gLastRxMs > 0 && (now - gLastRxMs) < 3500);
  if (isConn != wasConn) {
    wasConn = isConn; gConnected = isConn;
    gDirtyStatus = true;
  }

  // Modus-Timeouts
  if (gScanMode && now > gScanEndMs) {
    gScanMode = false; gDirtyRfid = true;
  }
  if (gWriteMode && now > gWriteEndMs) {
    gWriteMode = false; gWriteIdx = -1; gDirtyRfid = true;
  }

  // Countdown im Banner alle 500ms aktualisieren
  static unsigned long lastBannerMs = 0;
  if ((gScanMode || gWriteMode) && now - lastBannerMs > 500) {
    lastBannerMs = now; gDirtyRfid = true;
  }

  // Status alle 2s aktualisieren (Spannungs-Refresh)
  static unsigned long lastStRefMs = 0;
  if (now - lastStRefMs > 2000) {
    lastStRefMs = now; gDirtyStatus = true;
  }

  // Display neu zeichnen (nur dirty panels)
  drawAll(false);

  delay(20);
}

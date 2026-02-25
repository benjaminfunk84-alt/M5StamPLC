// M5Tab5 Controller – Touch-UI für Relay-Steuerung und RFID-Verwaltung
// Kommunikation mit CoreS3: WiFi (C6 via SDIO2) → UDP

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
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

// Tag erkannt – warte auf Bestätigung durch "SPEICHERN"-Button (10s)
bool          gTagFound    = false;
String        gFoundTag    = "";
unsigned long gTagFoundEndMs = 0;

// Write-Modus (10s – Tag auf Karte schreiben)
bool          gWriteMode  = false;
int           gWriteIdx   = -1;
unsigned long gWriteEndMs = 0;
bool          gWriteError = false;   // falscher Kartentyp
unsigned long gWriteErrMs = 0;
bool          gWriteOk    = false;   // Schreiben erfolgreich
unsigned long gWriteOkMs  = 0;

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
Rect rcSaveBtn;
Rect rcScrollUp, rcScrollDown;

// ============================================
// WiFi Station + UDP (Tab5 C6 via SDIO2 → CoreS3 SoftAP)
// SDIO2-Pins: P4 GPIO8-13 + Reset GPIO15
// ============================================
static const char*  AP_SSID         = "CoreS3-AP";
static const char*  AP_PASS         = "cores3pass";
static const int    UDP_STATUS_PORT = 4211;   // CoreS3 sendet Status
static const int    UDP_CMD_PORT    = 4210;   // CoreS3 empfängt Commands
static const size_t PMAX            = 512;

// Tab5-spezifische SDIO2-Pins (P4 → C6)
#define SDIO2_CLK  GPIO_NUM_12
#define SDIO2_CMD  GPIO_NUM_13
#define SDIO2_D0   GPIO_NUM_11
#define SDIO2_D1   GPIO_NUM_10
#define SDIO2_D2   GPIO_NUM_9
#define SDIO2_D3   GPIO_NUM_8
#define SDIO2_RST  GPIO_NUM_15

static WiFiUDP udpRx;   // Status empfangen + Commands senden (initialisierter Socket)

static void sendCmd(const char* json) {
  // udpRx (mit begin() initialisiert) für Broadcast-Versand verwenden
  static const IPAddress broadcastIP(192, 168, 4, 255);
  udpRx.beginPacket(broadcastIP, UDP_CMD_PORT);
  udpRx.print(json);
  udpRx.endPacket();
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

  // Write-Fehler (falscher Kartentyp)
  if (doc["wrerr"] | 0) {
    gWriteError  = true;
    gWriteErrMs  = millis();
    gDirtyRfid   = true;
  }
  // Write-Erfolg: Modus beenden, Erfolgs-Banner zeigen
  if (doc["wrok"] | 0) {
    gWriteOk   = true;
    gWriteOkMs = millis();
    gWriteMode = false;
    gWriteIdx  = -1;
    gDirtyRfid = true;
  }

  bool wasConnected = gConnected;
  gConnected = true;
  gLastRxMs  = millis();

  // Scan-Modus: Tag erkannt → Bestätigung durch SPEICHERN-Button abwarten
  if (gScanMode && millis() < gScanEndMs && gCurTag.length() > 0 && gCurTag != "-") {
    bool alreadySaved = false;
    for (int i = 0; i < (int)gTagCount; i++) { if (gTagList[i] == gCurTag) { alreadySaved = true; break; } }
    if (!alreadySaved && !gTagFound) {
      gTagFound      = true;
      gFoundTag      = gCurTag;
      gTagFoundEndMs = millis() + 10000;
      gScanMode      = false;
      sendCmd("{\"cmd\":\"rfid_scan_stop\"}");
      gDirtyRfid   = true;
      gDirtyStatus = true;
    }
  }

  if (relayChanged)                                        gDirtyRelay  = true;
  if (voltChanged || !wasConnected)                        gDirtyStatus = true;
  // Status-Leiste und RFID-Panel nur im Scan-Modus bei Tag-Wechsel aktualisieren
  if (tagChanged && gScanMode)                             gDirtyStatus = true;
}

static void readStatusUdp() {
  int n = udpRx.parsePacket();
  if (n <= 0 || n > (int)PMAX) return;
  char buf[PMAX + 1];
  int r = udpRx.read(buf, PMAX);
  buf[r] = '\0';
  processJsonLine(buf);
}

static void setupWiFi() {
  // SDIO2-Pins MÜSSEN vor WiFi.mode() gesetzt werden (Tab5-spezifisch!)
  WiFi.setPins(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);
  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID, AP_PASS);
  Serial.printf("WiFi: Verbinde mit %s ...\n", AP_SSID);
  udpRx.begin(UDP_STATUS_PORT);
  Serial.println("UDP RX bereit (Port 4211)");
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
  sprStatus.print(alive ? "WiFi verbunden" : "WiFi: verbinde...");

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

  // RFID-Status (nur im Scan-Modus aktuellen Tag zeigen)
  sprStatus.setCursor(650, 18);
  if (gScanMode) {
    sprStatus.setTextColor(C_ORANGE, C_PANEL);
    sprStatus.print("SCAN: ");
    sprStatus.setTextColor(gCurTag != "-" ? C_WHITE : C_DIMGREY, C_PANEL);
    sprStatus.print(gCurTag != "-" ? gCurTag : "warte...");
  } else {
    sprStatus.setTextColor(C_DIMGREY, C_PANEL);
    sprStatus.print("RFID: Scan-Taste druecken");
  }

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

  // ---- Banner: Scan-Modus / Tag-Gefunden / Write-Modus ----
  int bannerH = 0;
  unsigned long now = millis();
  rcSaveBtn = {0, 0, 0, 0};  // zurücksetzen
  if (gWriteOk && (now - gWriteOkMs < 3000)) {
    bannerH = 48;
    sprRfid.fillRoundRect(12, 30, ow - 24, bannerH, 8, (uint32_t)C_GREEN);
    sprRfid.setTextColor(C_BLACK, C_GREEN);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(20, 46);
    sprRfid.print("Erfolgreich geschrieben!");
  } else if (gWriteOk) {
    gWriteOk = false;  // Banner abgelaufen
  } else if (gTagFound && now < gTagFoundEndMs) {
    bannerH = 72;
    int rem = (int)((gTagFoundEndMs - now) / 1000) + 1;
    sprRfid.fillRoundRect(12, 30, ow - 24, bannerH, 8, (uint32_t)C_GREEN);
    sprRfid.setTextColor(C_BLACK, C_GREEN);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(20, 44);
    sprRfid.printf("Tag: %s", gFoundTag.c_str());
    // SPEICHERN-Button
    const int svW = 200, svH = 38;
    int svX = (ow - 24 - svW) / 2 + 12;
    int svY = 66;
    sprRfid.fillRoundRect(svX, svY, svW, svH, 8, (uint32_t)C_BLACK);
    sprRfid.drawRoundRect(svX, svY, svW, svH, 8, (uint32_t)C_WHITE);
    sprRfid.setTextColor(C_WHITE, C_BLACK);
    sprRfid.setTextSize(2);
    int btnTxtLen = snprintf(nullptr, 0, "SPEICHERN %ds", rem);
    sprRfid.setCursor(svX + (svW - btnTxtLen * 12) / 2, svY + 11);
    sprRfid.printf("SPEICHERN %ds", rem);
    rcSaveBtn = {px + svX, S_H + svY, svW, svH};
  } else if (gScanMode && now < gScanEndMs) {
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
    bool showErr = gWriteError && (now - gWriteErrMs < 3000);
    uint32_t bCol = showErr ? C_RED : C_ORANGE;
    sprRfid.fillRoundRect(12, 30, ow - 24, bannerH, 8, bCol);
    sprRfid.setTextColor(C_BLACK, bCol);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(20, 46);
    if (showErr) sprRfid.print("Falscher Kartentyp! MIFARE Classic");
    else         sprRfid.printf("Karte halten zum Beschreiben... %ds", rem);
  }

  // ---- Tag-Liste ----
  const int SCAN_BTN_H = 58;
  const int TAG_H      = 90;
  const int TAG_GAP    = 6;
  const int TAG_X      = 12;
  const int SCROLL_COL = 44;    // Breite der Scroll-Spalte rechts (eigene Zone)
  const int TAG_W      = ow - 24 - SCROLL_COL;  // Tags enden vor Scroll-Spalte
  const int LIST_Y     = 34 + bannerH + 4;
  int listH    = ph - LIST_Y - SCAN_BTN_H - 16;
  int maxVis   = max(1, listH / (TAG_H + TAG_GAP));

  // Scroll-Korrektur
  if (gTagScroll > (int)gTagCount - maxVis) gTagScroll = max(0, (int)gTagCount - maxVis);

  int visEnd = min((int)gTagCount, gTagScroll + maxVis);

  // Button-Dimensionen (Schreiben größer, mehr Abstand zu Löschen)
  const int dBtnW = 48, dBtnH = 40;  // Delete: quadratisch
  const int wBtnW = 120, wBtnH = 40; // Write: breiter
  const int btnGap = 20;             // Abstand zwischen den Buttons
  const int dBtnRightMargin = 8;

  for (int i = gTagScroll; i < visEnd; i++) {
    int cy = LIST_Y + (i - gTagScroll) * (TAG_H + TAG_GAP);
    bool isWriteTarget = (gWriteMode && gWriteIdx == i);
    uint32_t bg  = isWriteTarget ? 0x0D2B20 : C_CARD;
    uint32_t brd = isWriteTarget ? C_GREEN  : C_DIVIDER;

    sprRfid.fillRoundRect(TAG_X, cy, TAG_W, TAG_H, 8, bg);
    sprRfid.drawRoundRect(TAG_X, cy, TAG_W, TAG_H, 8, brd);

    // Button-Positionen (von rechts berechnet)
    int dBtnX = TAG_W - dBtnW - dBtnRightMargin;
    int wBtnX = dBtnX - btnGap - wBtnW;
    int btnY  = cy + (TAG_H - wBtnH) / 2;

    // Index-Label (größer)
    sprRfid.setTextSize(2);
    sprRfid.setTextColor(C_GREY, bg);
    sprRfid.setCursor(TAG_X + 14, cy + 10);
    sprRfid.printf("TAG %02d", i + 1);

    // Seriennummer-Label + UID (groß)
    sprRfid.setTextSize(1);
    sprRfid.setTextColor(C_DIMGREY, bg);
    sprRfid.setCursor(TAG_X + 14, cy + 44);
    sprRfid.print("SN:");
    sprRfid.setTextSize(2);
    sprRfid.setTextColor(C_CYAN, bg);
    sprRfid.setCursor(TAG_X + 42, cy + 58);
    // Zeilenumbruch falls zu lang (>14 Zeichen)
    String uid = gTagList[i];
    sprRfid.print(uid.length() > 14 ? uid.substring(0, 14) : uid);

    // Write-Button (cyan, größer)
    uint32_t wCol = isWriteTarget ? C_GREEN : C_CYAN;
    sprRfid.fillRoundRect(TAG_X + wBtnX, btnY, wBtnW, wBtnH, 8, wCol);
    sprRfid.setTextColor(C_BLACK, wCol);
    sprRfid.setTextSize(2);
    int wTxtX = TAG_X + wBtnX + (wBtnW - (isWriteTarget ? 9 : 8) * 12) / 2;
    sprRfid.setCursor(wTxtX, btnY + 12);
    sprRfid.print(isWriteTarget ? "ABBRUCH" : "SCHREIBEN");

    // Delete-Button (rot, quadratisch, mit Abstand)
    sprRfid.fillRoundRect(TAG_X + dBtnX, btnY, dBtnW, dBtnH, 8, (uint32_t)C_RED);
    sprRfid.setTextColor(C_WHITE, C_RED);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(TAG_X + dBtnX + 16, btnY + 12);
    sprRfid.print("X");

    // Hit-Areas in Display-Koordinaten
    rcTagCard[i] = {px + TAG_X,          S_H + cy,    wBtnX - 8,  TAG_H};
    rcTagWrt[i]  = {px + TAG_X + wBtnX,  S_H + btnY,  wBtnW, wBtnH};
    rcTagDel[i]  = {px + TAG_X + dBtnX,  S_H + btnY,  dBtnW, dBtnH};
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

  // Scroll-Buttons in eigener Spalte (SCROLL_COL, rechts neben TAG-Bereich)
  int scX = ow - SCROLL_COL + 4;  // x-Mitte der Scroll-Spalte
  int scW = SCROLL_COL - 8;
  if (gTagCount > maxVis) {
    // Nach oben (Dreieck in Scroll-Spalte, über der Liste)
    int upCy = LIST_Y + 16;
    uint32_t upC = (gTagScroll > 0) ? C_CYAN : C_DIMGREY;
    sprRfid.fillTriangle(scX + scW/2, upCy - 10,
                         scX + scW,   upCy + 10,
                         scX,         upCy + 10, upC);
    rcScrollUp = {px + scX, S_H + LIST_Y, scW, 36};

    // Nach unten
    int dnCy = LIST_Y + listH - 16;
    uint32_t dnC = (visEnd < (int)gTagCount) ? C_CYAN : C_DIMGREY;
    sprRfid.fillTriangle(scX + scW/2, dnCy + 10,
                         scX + scW,   dnCy - 10,
                         scX,         dnCy - 10, dnC);
    rcScrollDown = {px + scX, S_H + dnCy - 26, scW, 36};
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

  // SPEICHERN-Button (erscheint nach Tag-Erkennung im Scan-Modus)
  if (gTagFound && rcSaveBtn.hit(tx, ty)) {
    bool alreadySaved = false;
    for (int i = 0; i < (int)gTagCount; i++) { if (gTagList[i] == gFoundTag) { alreadySaved = true; break; } }
    if (!alreadySaved && gTagCount < 20) {
      gTagList[gTagCount++] = gFoundTag;
      sendCmd("{\"cmd\":\"rfid_learn\"}");
    }
    gTagFound  = false;
    gFoundTag  = "";
    gDirtyRfid = true;
    gDirtyStatus = true;
    return;
  }

  // Scan-Button
  if (rcScanBtn.hit(tx, ty)) {
    gScanMode = !gScanMode;
    gScanEndMs = gScanMode ? millis() + 5000 : 0;
    if (gScanMode) sendCmd("{\"cmd\":\"rfid_scan_start\"}");
    else           sendCmd("{\"cmd\":\"rfid_scan_stop\"}");
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
        // Schreib-Befehl mit UID an CoreS3 senden
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"write_tag\",\"uid\":\"%s\"}", gTagList[i].c_str());
        sendCmd(cmd);
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

  setupWiFi();
  drawAll(true);
  Serial.println("Setup complete");
}

// ============================================
// LOOP
// ============================================
void loop() {
  M5.update();
  unsigned long now = millis();

  // UDP Status von CoreS3 lesen
  readStatusUdp();

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
    gScanMode = false;
    sendCmd("{\"cmd\":\"rfid_scan_stop\"}");
    gDirtyRfid = true;
  }
  if (gTagFound && now > gTagFoundEndMs) {
    gTagFound = false; gFoundTag = ""; gDirtyRfid = true;
  }
  if (gWriteMode && now > gWriteEndMs) {
    gWriteMode = false; gWriteIdx = -1; gDirtyRfid = true;
  }

  // Countdown im Banner alle 500ms aktualisieren
  static unsigned long lastBannerMs = 0;
  if ((gScanMode || gTagFound || gWriteMode || gWriteOk) && now - lastBannerMs > 500) {
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

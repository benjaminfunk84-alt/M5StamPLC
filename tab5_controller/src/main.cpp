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

// Tag-Bibliothek (SD auf CoreS3, Darstellung/Steuerung auf Tab5)
struct TagLibItem {
  int    slot;
  String uid;
  String label;
};
TagLibItem gTagLib[64];
int        gTagLibCount      = 0;
bool       gTagLibDirty      = false;
bool       gTagLibOpen       = false;
bool       gTagLibLoadOk     = false;   // letzter Bibliotheks-Eintrag per "LADEN" gewählt
unsigned long gTagLibToggleMs = 0;      // Entprellung für Öffnen/Schließen der Bibliothek

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
unsigned long gPn532EmulEndMs = 0;  // Banner „Gerät an Reader halten“ nach SENDEN (PN532)
bool          gWriteError = false;   // falscher Kartentyp
unsigned long gWriteErrMs = 0;
bool          gWriteOk    = false;   // Schreiben erfolgreich
unsigned long gWriteOkMs  = 0;

// PN532 Card-Emulation (nur anzeigen wenn CoreS3 PN532 meldet)
bool          gPn532Present = false;
bool          gEmulActive   = false;
// Schreiben: RC522 (Legacy) oder CHARX 3150 per RS-485. PN532 = Emulation (Gerät an Reader halten).
bool          gRc522Present = false;   // Schreiben-Button (RC522 oder RS-485)
bool          gCharxRs485Present = false;  // UID direkt an CHARX 3150 (RS-485) sendbar
bool          gWritePathPn532 = true;     // true = PN532-Emulation, false = RS-485/CHARX (Umschalter)

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
Rect rcTagCard[20], rcTagDel[20], rcTagWrt[20], rcTagEmul[20];
Rect rcWritePathPn532, rcWritePathRs485;  // Umschalter PN532 / RS-485 unten links
Rect rcScanBtn;
Rect rcSaveBtn;
Rect rcScrollUp, rcScrollDown;
Rect rcTagLibBtn;           // Button zum Öffnen der Tag-Bibliothek
Rect rcTagLibClose;         // Schließen-Button im Bibliotheks-Menü
Rect rcTagLibLoad[64];
Rect rcTagLibDel[64];

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
  Serial.printf("Tab5 sendCmd → %s\n", json);
  udpRx.beginPacket(broadcastIP, UDP_CMD_PORT);
  udpRx.print(json);
  int rc = udpRx.endPacket();
  Serial.printf("Tab5 sendCmd endPacket rc=%d\n", rc);
}

// Forward decl
static void drawAll(bool full = false);
static void drawRfidPanel();
static void drawStatusBar();

static void processJsonLine(const char* buf) {
  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

  const char* cmd = doc["cmd"] | nullptr;
  if (cmd && strcmp(cmd, "taglib_list") == 0) {
    JsonArrayConst tArr = doc["tags"].as<JsonArrayConst>();
    gTagLibCount = 0;
    if (!tArr.isNull()) {
      for (JsonObjectConst o : tArr) {
        if (gTagLibCount >= 64) break;
        TagLibItem it;
        it.slot  = o["slot"]  | (int)(gTagLibCount + 1);
        it.uid   = String(o["uid"]   | "");
        it.label = String(o["label"] | "");
        if (it.uid.length() == 0) continue;
        gTagLib[gTagLibCount++] = it;
      }
    }
    // #region agent log
    Serial.printf("AGENT TAB H1: taglib_list received, count=%d rawSize=%d\n",
                  gTagLibCount, (int)tArr.size());
    // #endregion
    gTagLibDirty = true;
    return;
  }

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

  // Arbeitsliste (RFID-Tags im Hauptpanel)
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

  // Tag-Bibliothek wird jetzt bei jedem Status-Frame mitgesendet (Feld "taglib").
  // Dadurch ist die Bibliothek auch dann gefüllt, wenn der explizite taglib_get-
  // Befehl einmal nicht ankommt.
  JsonArrayConst libArr = doc["taglib"].as<JsonArrayConst>();
  if (!libArr.isNull()) {
    gTagLibCount = 0;
    for (JsonObjectConst o : libArr) {
      if (gTagLibCount >= 64) break;
      TagLibItem it;
      it.slot  = o["slot"]  | (int)(gTagLibCount + 1);
      it.uid   = String(o["uid"]   | "");
      it.label = String(o["label"] | "");
      if (it.uid.length() == 0) continue;
      gTagLib[gTagLibCount++] = it;
    }
    // #region agent log
    Serial.printf("AGENT TAB H2: taglib from status count=%d rawSize=%d\n",
                  gTagLibCount, (int)libArr.size());
    // #endregion
    gTagLibDirty = true;
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
  gPn532Present = (doc["pn532"] | 0) != 0;
  gRc522Present = (doc["rc522"] | 0) != 0;  // Schreiben (RC522 oder RS-485)
  gCharxRs485Present = (doc["charx_rs485"] | 0) != 0;  // Ziel CHARX 3150 per RS-485
  gEmulActive   = (doc["emul"] | 0) != 0;
  if (gPn532Present || gEmulActive) { gDirtyRfid = true; gDirtyStatus = true; }
  if (gPn532Present || gCharxRs485Present) gDirtyRelay = true;  // Umschalter anzeigen

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

  // RFID-Status (Scan / Emulation / normal)
  sprStatus.setCursor(650, 18);
  if (gEmulActive) {
    sprStatus.setTextColor(C_GREEN, C_PANEL);
    sprStatus.print("Emulation aktiv");
  } else if (gScanMode) {
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

  // Umschalter PN532 / RS-485 unten links (nur wenn mindestens ein Weg verfügbar)
  const int SWITCH_BAR_H = 52;
  if (gPn532Present || gCharxRs485Present) {
    int swY = ph - SWITCH_BAR_H - 6;  // 6px Abstand zum unteren Rand
    int halfW = pw / 2;
    sprRelay.drawFastHLine(0, swY - 2, pw, C_DIVIDER);
    // Ausgewählte Seite immer cyan (wie RS-485), unabhängig von gPn532Present
    uint32_t pnBg = gWritePathPn532 ? C_CYAN : C_DIMGREY;
    uint32_t rsBg = !gWritePathPn532 ? C_CYAN : C_DIMGREY;
    if (!gCharxRs485Present) rsBg = C_DIMGREY;  // RS-485 nur grau wenn nicht verfügbar
    sprRelay.fillRoundRect(0, swY, halfW, SWITCH_BAR_H, 8, pnBg);
    sprRelay.fillRoundRect(halfW, swY, halfW, SWITCH_BAR_H, 8, rsBg);
    sprRelay.setTextColor(C_BLACK, pnBg);
    sprRelay.setTextSize(2);
    sprRelay.setCursor(20, swY + 16);
    sprRelay.print("PN532");
    sprRelay.setTextColor(C_BLACK, rsBg);
    sprRelay.setCursor(halfW + 20, swY + 16);
    sprRelay.print("RS-485");
    rcWritePathPn532 = {0, S_H + swY, halfW, SWITCH_BAR_H};
    rcWritePathRs485 = {halfW, S_H + swY, halfW, SWITCH_BAR_H};
  } else {
    rcWritePathPn532 = rcWritePathRs485 = {0, 0, 0, 0};
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
  sprRfid.setTextSize(2);          // etwas größer
  sprRfid.setCursor(16, 10);
  sprRfid.print(hdr);

  // Button: TAG-SPEICHER (öffnet Bibliotheks-Menü)
  const char* libTxt = "TAG-SPEICHER";
  int libW = 234, libH = 36;  // ~30 % größer
  int libX = ow - libW - 16;
  int libY = 8;
  sprRfid.fillRoundRect(libX, libY, libW, libH, 8, (uint32_t)C_CYAN);
  sprRfid.setTextColor(C_BLACK, C_CYAN);
  sprRfid.setTextSize(2);
  sprRfid.setCursor(libX + 12, libY + 8);
  sprRfid.print(libTxt);
  rcTagLibBtn = {px + libX, S_H + libY, libW, libH};

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
    else if (gCharxRs485Present) sprRfid.printf("UID an CHARX 3150 (RS-485)... %ds", rem);
    else if (gPn532Present) sprRfid.printf("Gerät mit PN532 an Reader halten... %ds", rem);
    else sprRfid.printf("Karte auflegen... %ds", rem);
  } else if (gPn532EmulEndMs > 0 && now < gPn532EmulEndMs) {
    bannerH = 48;
    int rem = (int)((gPn532EmulEndMs - now) / 1000) + 1;
    sprRfid.fillRoundRect(12, 30, ow - 24, bannerH, 8, (uint32_t)C_ORANGE);
    sprRfid.setTextColor(C_BLACK, C_ORANGE);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(20, 46);
    sprRfid.printf("Gerät an Reader halten (PN532)... %ds", rem);
  }

  // ---- Tag-Liste ----
  const int SCAN_BTN_H = 58;
  const int TAG_H      = 90;
  const int TAG_GAP    = 6;
  const int TAG_X      = 12;
  const int SCROLL_COL = 44;    // Breite der Scroll-Spalte rechts (eigene Zone)
  const int TAG_W      = ow - 24 - SCROLL_COL;  // Tags enden vor Scroll-Spalte
  const int LIST_Y     = 56 + bannerH;   // mehr Abstand unterhalb TAG-SPEICHER-Button
  int listH    = ph - LIST_Y - SCAN_BTN_H - 16;
  int maxVis   = max(1, listH / (TAG_H + TAG_GAP));

  // Scroll-Korrektur
  if (gTagScroll > (int)gTagCount - maxVis) gTagScroll = max(0, (int)gTagCount - maxVis);

  int visEnd = min((int)gTagCount, gTagScroll + maxVis);

  // Alte Hit-Rects löschen, damit nur sichtbare Tags reagieren
  for (int j = 0; j < 20; j++) {
    rcTagWrt[j] = {0, 0, 0, 0};
    rcTagEmul[j] = {0, 0, 0, 0};
  }

  // Button-Dimensionen: ein SENDEN-Button (weiter links, mehr Abstand zum Löschen-Button)
  const int dBtnW = 48, dBtnH = 40;   // Delete
  const int sBtnW = 100, sBtnH = 40;  // SENDEN
  const int btnGap = 28;              // Abstand SENDEN ↔ Löschen (größer = SENDEN weiter links)
  const int dBtnRightMargin = 8;
  bool showSendBtn = gPn532Present || gCharxRs485Present;

  for (int i = gTagScroll; i < visEnd; i++) {
    int cy = LIST_Y + (i - gTagScroll) * (TAG_H + TAG_GAP);
    bool isWriteTarget = (gWriteMode && gWriteIdx == i);
    uint32_t bg  = isWriteTarget ? 0x0D2B20 : C_CARD;
    uint32_t brd = isWriteTarget ? C_GREEN  : C_DIVIDER;

    sprRfid.fillRoundRect(TAG_X, cy, TAG_W, TAG_H, 8, bg);
    sprRfid.drawRoundRect(TAG_X, cy, TAG_W, TAG_H, 8, brd);

    // Von rechts: Del, SENDEN (ein Button)
    int dBtnX = TAG_W - dBtnW - dBtnRightMargin;
    int sBtnX = dBtnX - btnGap - sBtnW;
    int btnY  = cy + (TAG_H - sBtnH) / 2;

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
    String uid = gTagList[i];
    sprRfid.print(uid.length() > 14 ? uid.substring(0, 14) : uid);

    // Ein SENDEN-Button (nutzt PN532 oder RS-485 je nach Umschalter)
    if (showSendBtn) {
      uint32_t sCol = isWriteTarget ? C_GREEN : (gWritePathPn532 ? C_ORANGE : C_CYAN);
      const char* sLabel = isWriteTarget ? "ABBR." : "SENDEN";
      sprRfid.fillRoundRect(TAG_X + sBtnX, btnY, sBtnW, sBtnH, 8, sCol);
      sprRfid.setTextColor(C_BLACK, sCol);
      sprRfid.setTextSize(2);
      int sTxtW = strlen(sLabel) * 12;
      sprRfid.setCursor(TAG_X + sBtnX + (sBtnW - sTxtW) / 2, btnY + 10);
      sprRfid.print(sLabel);
      rcTagWrt[i]  = {px + TAG_X + sBtnX, S_H + btnY, sBtnW, sBtnH};
    } else {
      rcTagWrt[i] = {0, 0, 0, 0};
    }

    // Delete-Button (rot, quadratisch)
    sprRfid.fillRoundRect(TAG_X + dBtnX, btnY, dBtnW, dBtnH, 8, (uint32_t)C_RED);
    sprRfid.setTextColor(C_WHITE, C_RED);
    sprRfid.setTextSize(2);
    sprRfid.setCursor(TAG_X + dBtnX + 16, btnY + 12);
    sprRfid.print("X");

    rcTagCard[i] = {px + TAG_X, S_H + cy, showSendBtn ? sBtnX - 8 : TAG_W - 8, TAG_H};
    rcTagDel[i]  = {px + TAG_X + dBtnX, S_H + btnY, dBtnW, dBtnH};
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
// ZEICHNEN – Tag-Bibliothek (Overlay)
// ============================================
static void drawTagLibOverlay() {
  if (!gTagLibOpen) return;
  int px = L_W + 1;
  int pw = DW - px;
  int ph = DH - S_H;
  unsigned long now = millis();

  // Halbtransparentes Overlay über dem RFID-Panel
  LGFX_Sprite overlay(nullptr);
  overlay.setPsram(true);
  overlay.createSprite(pw, ph);
  overlay.fillRect(0, 0, pw, ph, (uint32_t)C_BLACK);
  overlay.setPaletteColor(0, C_BLACK);

  // Hintergrund für Bibliothek
  overlay.fillRoundRect(8, 8, pw - 16, ph - 16, 12, (uint32_t)C_PANEL);

  // Titel
  overlay.setTextColor(C_CYAN, C_PANEL);
  overlay.setTextSize(3);  // etwas größer
  overlay.setCursor(24, 24);
  overlay.print("TAG-BIBLIOTHEK");

  // Schliessen-Button
  int cbW = 130, cbH = 42;  // ~30 % größer
  int cbX = pw - cbW - 24;
  int cbY = 20;
  overlay.fillRoundRect(cbX, cbY, cbW, cbH, 8, (uint32_t)C_RED);
  overlay.setTextColor(C_WHITE, C_RED);
  overlay.setTextSize(2);
  overlay.setCursor(cbX + 14, cbY + 10);
  overlay.print("Zurueck");
  rcTagLibClose = {px + cbX, S_H + cbY, cbW, cbH};

  // Kurzer Hinweis nach erfolgreichem Laden
  if (gTagLibLoadOk) {
    overlay.setTextColor(C_GREEN, C_PANEL);
    overlay.setTextSize(2);
    overlay.setCursor(24, 72);
    overlay.print("Tag geladen");
  }

  // Liste der Bibliotheks-Tags
  int listY = 88;
  int rowH  = 52;  // höher, Buttons/Schrift ~30 % größer
  for (int i = 0; i < gTagLibCount && i < 64; ++i) {
    int y = listY + i * rowH;
    if (y + rowH + 8 > ph - 16) break;
    uint32_t bg = (uint32_t)C_CARD;
    overlay.fillRoundRect(16, y, pw - 32, rowH - 4, 8, bg);

    overlay.setTextColor(C_GREY, bg);
    overlay.setTextSize(2);
    overlay.setCursor(24, y + 8);
    overlay.printf("%s", gTagLib[i].label.c_str());
    overlay.setCursor(24, y + 30);
    overlay.setTextColor(C_CYAN, bg);
    String uid = gTagLib[i].uid;
    overlay.print(uid.length() > 18 ? uid.substring(0, 18) : uid);

    // Load-Button
    int lbW = 104, lbH = 34;  // ~30 % größer
    int lbX = pw - lbW - 120;
    int lbY = y + 8;
    overlay.fillRoundRect(lbX, lbY, lbW, lbH, 6, (uint32_t)C_CYAN);
    overlay.setTextColor(C_BLACK, C_CYAN);
    overlay.setTextSize(2);
    overlay.setCursor(lbX + 10, lbY + 7);
    overlay.print("LADEN");
    rcTagLibLoad[i] = {px + lbX, S_H + lbY, lbW, lbH};

    // Delete-Button
    int dbW = 52, dbH = 34;  // ~30 % größer
    int dbX = pw - dbW - 40;
    int dbY = lbY;
    overlay.fillRoundRect(dbX, dbY, dbW, dbH, 6, (uint32_t)C_RED);
    overlay.setTextColor(C_WHITE, C_RED);
    overlay.setTextSize(2);
    overlay.setCursor(dbX + 14, dbY + 7);
    overlay.print("X");
    rcTagLibDel[i] = {px + dbX, S_H + dbY, dbW, dbH};
  }

  overlay.pushSprite(&M5.Display, px, S_H);
  overlay.deleteSprite();
  gTagLibDirty = false;
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
  // Wenn die Tag-Bibliothek geöffnet ist, zeichnet nur das Overlay den rechten Bereich.
  // Dadurch kann es nicht mehr von darunterliegenden Panels "übermalt" werden.
  if (gTagLibOpen) {
    drawTagLibOverlay();
    return;
  }

  if (gDirtyStatus) drawStatusBar();
  if (gDirtyRelay)  drawRelayPanel();
  if (gDirtyRfid)   drawRfidPanel();
}

// ============================================
// TOUCH HANDLING
// ============================================
static void handleTouch(int tx, int ty) {
  // #region agent log
  Serial.printf("AGENT TAB H3: handleTouch tx=%d ty=%d gTagLibOpen=%d\n",
                tx, ty, gTagLibOpen ? 1 : 0);
  // #endregion

  unsigned long now = millis();

  // Zuerst: Klicks im Tag-Bibliotheks-Overlay abfangen, solange es geöffnet ist
  if (gTagLibOpen) {
    if (rcTagLibClose.hit(tx, ty)) {
      // Entprellung: Mehrfache Close-Taps in kurzer Zeit ignorieren
      if (now - gTagLibToggleMs < 400) return;
      gTagLibToggleMs = now;
      gTagLibOpen   = false;
      gTagLibLoadOk = false;
      gTagLibDirty  = true;
      Serial.println("Tab5 Touch: TAG-BIBLIOTHEK schliessen");
      return;
    }
    // Einträge: Laden / Löschen
    for (int i = 0; i < gTagLibCount && i < 64; ++i) {
      if (rcTagLibLoad[i].hit(tx, ty)) {
        char cmd[80];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"taglib_load\",\"slot\":%d}", gTagLib[i].slot);
        sendCmd(cmd);
        Serial.printf("Tab5 Touch: TAG-LIB LOAD slot=%d uid=%s\n",
                      gTagLib[i].slot, gTagLib[i].uid.c_str());
        gTagLibLoadOk = true;   // Hinweis im Overlay anzeigen
        gTagLibDirty  = true;
        return;
      }
      if (rcTagLibDel[i].hit(tx, ty)) {
        char cmd[80];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"taglib_delete\",\"slot\":%d}", gTagLib[i].slot);
        sendCmd(cmd);
        Serial.printf("Tab5 Touch: TAG-LIB DELETE slot=%d uid=%s\n",
                      gTagLib[i].slot, gTagLib[i].uid.c_str());
        return;
      }
    }
    // Wenn Overlay offen ist und kein Bibliotheks-Element getroffen wurde,
    // sollen keine weiteren Aktionen ausgelöst werden.
    return;
  }

  // Scroll
  if (rcScrollUp.hit(tx, ty))   {
    gTagScroll = max(0, gTagScroll - 1);
    gDirtyRfid = true;
    Serial.printf("Tab5 Touch: ScrollUp -> gTagScroll=%d\n", gTagScroll);
    return;
  }
  if (rcScrollDown.hit(tx, ty)) {
    gTagScroll++;
    gDirtyRfid = true;
    Serial.printf("Tab5 Touch: ScrollDown -> gTagScroll=%d\n", gTagScroll);
    return;
  }

  // SPEICHERN-Button (erscheint nach Tag-Erkennung im Scan-Modus)
  if (gTagFound && rcSaveBtn.hit(tx, ty)) {
    bool alreadySaved = false;
    for (int i = 0; i < (int)gTagCount; i++) { if (gTagList[i] == gFoundTag) { alreadySaved = true; break; } }
    if (!alreadySaved && gTagCount < 20) {
      Serial.printf("Tab5 Touch: SAVE tag=%s (index=%d)\n",
                    gFoundTag.c_str(), (int)gTagCount);
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
    Serial.printf("Tab5 Touch: SCAN %s\n", gScanMode ? "ON" : "OFF");
    if (gScanMode) sendCmd("{\"cmd\":\"rfid_scan_start\"}");
    else           sendCmd("{\"cmd\":\"rfid_scan_stop\"}");
    gDirtyRfid = true;
    return;
  }

  // Tag-Bibliothek öffnen
  if (rcTagLibBtn.hit(tx, ty)) {
    // Entprellung: Mehrfache Open-Taps in kurzer Zeit ignorieren
    if (now - gTagLibToggleMs < 400) return;
    gTagLibToggleMs = now;
    gTagLibOpen  = true;
    gTagLibDirty = true;
    // Beim Öffnen aktuelle Bibliothek vom CoreS3 anfordern
    sendCmd("{\"cmd\":\"taglib_get\"}");
    // #region agent log
    Serial.printf("AGENT TAB H3: TAG-BIBLIOTHEK öffnen tx=%d ty=%d\n", tx, ty);
    // #endregion
    Serial.println("Tab5 Touch: TAG-BIBLIOTHEK öffnen");
    return;
  }

  // Relay-Karten
  if (gPn532Present || gCharxRs485Present) {
    if (rcWritePathPn532.hit(tx, ty)) {
      gWritePathPn532 = true;
      gDirtyRelay = true;
      Serial.println("Tab5 Touch: PATH = PN532");
      return;
    }
    if (rcWritePathRs485.hit(tx, ty)) {
      gWritePathPn532 = false;
      gDirtyRelay = true;
      Serial.println("Tab5 Touch: PATH = RS485");
      return;
    }
  }
  for (int i = 0; i < 4; i++) {
    if (rcRelay[i].hit(tx, ty)) {
      char cmd[64];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"set_relay\",\"idx\":%d,\"val\":%d}", i, gRelay[i] ? 0 : 1);
      sendCmd(cmd);
      gRelay[i]   = !gRelay[i];  // Optimistisches Update
      gDirtyRelay = true;
      Serial.printf("Tab5 Touch: RELAY idx=%d -> %d\n", i, gRelay[i] ? 1 : 0);
      return;
    }
  }

  // Tag-Buttons (nur sichtbare – gleiche Formel wie im Draw)
  const int TAG_H = 90, TAG_GAP = 6, SCAN_BTN_H = 58, LIST_Y_BASE = 34 + 4;
  int listH = (DH - S_H) - LIST_Y_BASE - SCAN_BTN_H - 16;
  int maxVis = max(1, listH / (TAG_H + TAG_GAP));
  int visEnd = min((int)gTagCount, gTagScroll + maxVis);
  for (int i = gTagScroll; i < visEnd && i < 20; i++) {
    // Löschen
    if (rcTagDel[i].hit(tx, ty)) {
      char cmd[128];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"rfid_delete\",\"uid\":\"%s\"}", gTagList[i].c_str());
      sendCmd(cmd);
      Serial.printf("Tab5 Touch: DELETE idx=%d uid=%s\n", i, gTagList[i].c_str());
      for (int j = i; j < gTagCount - 1; j++) gTagList[j] = gTagList[j + 1];
      if (gTagCount > 0) gTagCount--;
      if (gTagScroll > 0 && gTagScroll >= (int)gTagCount) gTagScroll--;
      gWriteMode = false; gWriteIdx = -1;
      gDirtyRfid = true;
      return;
    }
    // SENDEN (ein Button: PN532 oder RS-485 je nach Umschalter)
    if (rcTagWrt[i].hit(tx, ty)) {
      if (!(gPn532Present || gCharxRs485Present)) return;
      // PN532: rfid_emulate senden + kurzes Banner (wie Karte kurz anhalten)
      if (gWritePathPn532) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"rfid_emulate\",\"uid\":\"%s\"}", gTagList[i].c_str());
        sendCmd(cmd);
        gPn532EmulEndMs = millis() + 3000;  // 3 s Anzeige wie PN532-Emulation auf CoreS3
        gWriteMode = false;
        gWriteIdx  = -1;
        gDirtyRfid = true;
        Serial.printf("Tab5 Touch: SEND idx=%d via PN532 uid=%s\n", i, gTagList[i].c_str());
        return;
      }
      // RS-485: Abbrechen oder UID senden
      if (gCharxRs485Present) {
        if (gWriteMode && gWriteIdx == i) {
          gWriteMode = false;
          gWriteIdx  = -1;
          gDirtyRfid = true;
          Serial.printf("Tab5 Touch: SEND RS485 idx=%d -> CANCEL\n", i);
          return;
        }
        gWriteMode  = true;
        gWriteIdx   = i;
        gWriteEndMs = millis() + 10000;
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"write_tag\",\"uid\":\"%s\",\"target\":\"rs485\"}", gTagList[i].c_str());
        sendCmd(cmd);
        Serial.printf("Tab5 Touch: SEND idx=%d via RS485 uid=%s\n", i, gTagList[i].c_str());
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
  if (gPn532EmulEndMs > 0 && now > gPn532EmulEndMs) {
    gPn532EmulEndMs = 0; gDirtyRfid = true;
  }

  // Countdown im Banner alle 500ms aktualisieren
  static unsigned long lastBannerMs = 0;
  if ((gScanMode || gTagFound || gWriteMode || gWriteOk || gPn532EmulEndMs > 0) && now - lastBannerMs > 500) {
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

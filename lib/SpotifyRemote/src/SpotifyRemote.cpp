#include "SpotifyRemote.h"
#include <NimBLEDevice.h>
#include <WiFi.h>

#define SCREEN_W   240
#define SCREEN_H   240

#define COLOR_BG 0x0000 // GC9A01A_BLACK
#define COLOR_FG 0xFFFF // GC9A01A_WHITE

// ─── BLE UUIDs ───────────────────────────────────────────────
#define SVC_UUID          "12345678-1234-1234-1234-1234567890ab"
#define CHR_TITLE         "12345678-1234-1234-1234-1234567890a1"
#define CHR_ARTIST        "12345678-1234-1234-1234-1234567890a2"
#define CHR_DURATION      "12345678-1234-1234-1234-1234567890a3"
#define CHR_POSITION      "12345678-1234-1234-1234-1234567890a4"
#define CHR_CONTROL       "12345678-1234-1234-1234-1234567890a5"
#define CHR_STATUS        "12345678-1234-1234-1234-1234567890a6"
#define CHR_LYRIC_PREV    "12345678-1234-1234-1234-1234567890b1"
#define CHR_LYRIC_ACTIVE  "12345678-1234-1234-1234-1234567890b2"
#define CHR_LYRIC_NEXT    "12345678-1234-1234-1234-1234567890b3"

// ─── BLE objects ─────────────────────────────────────────────
static NimBLEServer*         bleServer      = nullptr;
static NimBLECharacteristic* chrControl     = nullptr;
static bool bleConnected = false;

// ─── Player state ────────────────────────────────────────────
static char     songTitle[64]  = "No Track";
static char     songArtist[64] = "Unknown";
static uint32_t songDuration   = 0;
static uint32_t songPosition   = 0;
static bool     isPlaying      = false;

static unsigned long lastPosUpdate = 0;
static uint32_t localPosition = 0;
static float    animPosition  = 0;
static float    lerpSpeed     = 0.25;

// ─── UI state ────────────────────────────────────────────────
enum UIView { VIEW_PLAYER, VIEW_LYRICS };
static UIView currentView = VIEW_PLAYER;

enum ControlCmd { CTRL_BACK, CTRL_MENU, CTRL_PREV, CTRL_PAUSE, CTRL_NEXT };
static ControlCmd selectedCtrl = CTRL_PAUSE;

static char lyricActPrev[64] = "";
static char lyricActive[64] = "";
static char lyricNext[64]   = "";
static char currentLyric[64] = "";
static char outgoingLyric[64] = "";
static float lyricAnimY = 0.0f;

static int      scrollX      = SCREEN_W;
static unsigned long lastScrollTime = 0;
static int      titlePixelW  = 0;

static void sendControlCommand(ControlCmd c) {
  if (!bleConnected || chrControl == nullptr) return;
  uint8_t cmd = (c == CTRL_PREV) ? 0 : (c == CTRL_PAUSE) ? 1 : 2;
  chrControl->setValue(&cmd, 1);
  chrControl->notify();
  Serial.printf("CMD sent: %d (%s)\n", cmd,
    c == CTRL_PREV ? "PREV" : c == CTRL_PAUSE ? "PLAY/PAUSE" : "NEXT");
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
    bleConnected = true;
    Serial.println("BLE Connected");
    pServer->updateConnParams(desc->conn_handle, 6, 12, 0, 400);
  }
  void onDisconnect(NimBLEServer* pServer) override {
    bleConnected = false;
    Serial.println("BLE Disconnected");
    NimBLEDevice::startAdvertising();
  }
};

class TitleCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    std::string val = pChr->getValue();
    strncpy(songTitle, val.c_str(), sizeof(songTitle) - 1);
    songTitle[sizeof(songTitle) - 1] = '\0';
    scrollX = SCREEN_W;
  }
};

class ArtistCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    std::string val = pChr->getValue();
    strncpy(songArtist, val.c_str(), sizeof(songArtist) - 1);
    songArtist[sizeof(songArtist) - 1] = '\0';
  }
};

class DurationCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    if (pChr->getValue().length() >= 4) {
      uint8_t* d = (uint8_t*)pChr->getValue().data();
      songDuration = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                     ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    }
  }
};

class PositionCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    if (pChr->getValue().length() >= 4) {
      uint8_t* d = (uint8_t*)pChr->getValue().data();
      localPosition = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                      ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
      lastPosUpdate = millis();
    }
  }
};

class StatusCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    if (pChr->getValue().length() >= 1) {
      isPlaying = (pChr->getValue()[0] == 1);
    }
  }
};

class LyricPrevCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    std::string val = pChr->getValue();
    strncpy(lyricActPrev, val.c_str(), sizeof(lyricActPrev) - 1);
    lyricActPrev[sizeof(lyricActPrev) - 1] = '\0';
  }
};

class LyricActiveCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    std::string val = pChr->getValue();
    strncpy(lyricActive, val.c_str(), sizeof(lyricActive) - 1);
    lyricActive[sizeof(lyricActive) - 1] = '\0';
  }
};

class LyricNextCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr) override {
    std::string val = pChr->getValue();
    strncpy(lyricNext, val.c_str(), sizeof(lyricNext) - 1);
    lyricNext[sizeof(lyricNext) - 1] = '\0';
  }
};

void SpotifyRemoteClass::begin() {
  // To prevent abort() when switching from WiFi to BLE on ESP32-S3:
  // We must ensure WiFi is properly disconnected and we yield so 
  // the network stack is cleaned up before BLE controller init.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  NimBLEDevice::init("Spotfy-ESP32");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  
  if (bleServer != nullptr) {
      NimBLEDevice::getAdvertising()->start();
      return;
  }
  
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pSvc = bleServer->createService(SVC_UUID);

  auto chrTitle = pSvc->createCharacteristic(CHR_TITLE, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrTitle->setCallbacks(new TitleCallback());
  auto chrArtist = pSvc->createCharacteristic(CHR_ARTIST, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrArtist->setCallbacks(new ArtistCallback());
  auto chrDuration = pSvc->createCharacteristic(CHR_DURATION, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrDuration->setCallbacks(new DurationCallback());
  auto chrPosition = pSvc->createCharacteristic(CHR_POSITION, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrPosition->setCallbacks(new PositionCallback());
  
  chrControl = pSvc->createCharacteristic(CHR_CONTROL, NIMBLE_PROPERTY::NOTIFY);
  
  auto chrStatus = pSvc->createCharacteristic(CHR_STATUS, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrStatus->setCallbacks(new StatusCallback());

  auto chrLyricP = pSvc->createCharacteristic(CHR_LYRIC_PREV, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrLyricP->setCallbacks(new LyricPrevCallback());
  auto chrLyricA = pSvc->createCharacteristic(CHR_LYRIC_ACTIVE, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrLyricA->setCallbacks(new LyricActiveCallback());
  auto chrLyricN = pSvc->createCharacteristic(CHR_LYRIC_NEXT, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrLyricN->setCallbacks(new LyricNextCallback());

  pSvc->start();
  
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SVC_UUID);
  pAdv->setScanResponse(true);
  pAdv->start();
}

void SpotifyRemoteClass::end() {
  if (bleServer != nullptr) {
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::deinit(true);
    bleServer = nullptr;
    bleConnected = false;
    Serial.println("SpotifyRemote: BLE de-initialized for WiFi coexistence.");
  }
}

static void drawPlayerUI(Adafruit_GFX* display) {
  display->fillScreen(COLOR_BG);
  display->setTextWrap(false);
  display->setTextSize(1);
  display->setTextColor(COLOR_FG);

  if (selectedCtrl == CTRL_BACK) {
    display->fillRect(0, 0, 14, 9, COLOR_FG);
    display->setTextColor(COLOR_BG);
  } else {
    display->setTextColor(COLOR_FG);
  }
  display->setCursor(2, 1);
  display->print("<");

  if (selectedCtrl == CTRL_MENU) {
    display->fillRect(SCREEN_W - 20, 0, 20, 9, COLOR_FG);
    display->setTextColor(COLOR_BG);
  } else {
    display->setTextColor(COLOR_FG);
  }
  display->setCursor(SCREEN_W - 17, 1);
  display->print("...");

  display->setTextColor(COLOR_FG);
  if (bleConnected) {
    display->setCursor(58, 1); display->print("BT");
  } else {
    display->setCursor(54, 1); display->print("..."); 
  }
  
  display->drawLine(0, 30, SCREEN_W, 30, COLOR_FG);

  if (currentView == VIEW_PLAYER) {
    display->setTextSize(2);
    titlePixelW = strlen(songTitle) * 12;
    if (titlePixelW <= SCREEN_W - 20) {
      int16_t x = (SCREEN_W - titlePixelW) / 2;
      display->setCursor(x, 60);
    } else {
      display->setCursor(scrollX, 60);
    }
    display->print(songTitle);

    display->setTextSize(1);
    int artistW = strlen(songArtist) * 6;
    int16_t ax = (SCREEN_W - artistW) / 2;
    if (ax < 0) ax = 0;
    display->setCursor(ax, 90);
    display->print(songArtist);

    // Draw active lyric line above progress bar with push-up animation
    display->setTextSize(1);
    display->setTextColor(COLOR_FG);

    auto drawWrapped = [&](const char* text, int yOffset) {
        char buf[64];
        strncpy(buf, text, 63); buf[63] = '\0';
        int len = strlen(buf);
        
        // Handle Ellipsis for very long lyrics
        if (len > 55) {
            strcpy(buf + 52, "...");
            len = 55;
        }

        if (len <= 28) {
            display->setCursor(30, yOffset);
            display->print(buf);
        } else {
            int split = 28;
            for (int i = 28; i > 10; i--) {
                if (buf[i] == ' ') { split = i; break; }
            }
            char line1[35] = {0};
            strncpy(line1, buf, split);
            display->setCursor(30, yOffset - 5);
            display->print(line1);

            char line2[35] = {0};
            strncpy(line2, buf + split + 1, 28);
            if (strlen(buf + split + 1) > 28) strcat(line2, "...");
            display->setCursor(30, yOffset + 7);
            display->print(line2);
        }
    };

    if (lyricAnimY > 0.0f && strlen(outgoingLyric) > 0 && strcmp(outgoingLyric, "No lyrics") != 0) {
        int drawY_old = 115 - (15 - (int)lyricAnimY);
        drawWrapped(outgoingLyric, drawY_old);
    }

    if (strlen(currentLyric) > 0 && strcmp(currentLyric, "No lyrics") != 0) {
        int drawY_new = 115 + (int)lyricAnimY;
        drawWrapped(currentLyric, drawY_new);
    }

    // MASKING: Draw black cover bars to make lyrics "disappear" into the background
    // Covers above the lyric area (Artist ends at ~98)
    display->fillRect(0, 98, SCREEN_W, 12, COLOR_BG); 
    // Covers below the lyric area (Bar starts at 140)
    display->fillRect(0, 133, SCREEN_W, 7, COLOR_BG);
  }

  uint32_t dur = (songDuration > 0) ? songDuration : 1;
  float pos = (animPosition <= dur) ? animPosition : (float)dur;
  int barW = 180, barX = 30, barY = 140, barH = 6;
  display->drawRect(barX, barY, barW, barH, COLOR_FG);
  int fillW = (int)((pos / (float)dur) * barW);
  if (fillW > 0) display->fillRect(barX, barY, fillW, barH, COLOR_FG);
  display->fillCircle(barX + fillW, barY + barH / 2, 4, COLOR_FG);

  struct CtrlBtn { ControlCmd ctrl; int x; } btns[] = {
    { CTRL_PREV, 60 }, { CTRL_PAUSE, 110 }, { CTRL_NEXT, 160 }
  };

  for (auto& b : btns) {
    bool sel = (b.ctrl == selectedCtrl);
    int bx = b.x, by = 165, bw = 30, bh = 20;
    if (sel) {
      display->fillRoundRect(bx - 3, by - 3, bw + 6, bh + 6, 5, COLOR_FG);
      display->setTextColor(COLOR_BG);
    } else {
      display->setTextColor(COLOR_FG);
    }
    
    const char* sym = (b.ctrl == CTRL_PREV) ? "|<" : (b.ctrl == CTRL_NEXT) ? ">|" : (isPlaying ? "||" : ">");
    int sw = strlen(sym) * 6;
    display->setCursor(bx + (bw - sw) / 2, by + 6);
    display->print(sym);
    display->setTextColor(COLOR_FG);
  }
}

static void drawLyricsUI(Adafruit_GFX* display) {
  display->fillScreen(COLOR_BG);
  display->setTextWrap(false);

  auto printCenter = [&](const char* text, int y, int size) {
    display->setTextSize(size);
    int w = strlen(text) * 6 * size;
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    display->setCursor(x, y); 
    display->print(text);
  };

  if (strlen(lyricActPrev) > 0) {
    display->setTextColor(COLOR_FG);
    printCenter(lyricActPrev, 40, 1);
  }

  if (strlen(lyricActive) == 0) {
    display->setTextColor(COLOR_FG);
    printCenter("No lyrics", 110, 1);
  } else {
    display->setTextColor(COLOR_FG);
    char buf[64]; strncpy(buf, lyricActive, 63); buf[63] = '\0';
    int len = strlen(buf), lineIdx = 0, start = 0, startY = 90, lineH = 20;
    while (start < len && lineIdx < 3) {
      int end = start + 18;
      if (end >= len) end = len;
      else {
        int sp = end;
        while (sp > start && buf[sp] != ' ') sp--;
        if (sp > start) end = sp;
      }
      char seg[22] = {0}; strncpy(seg, buf + start, end - start);
      char* trimmed = seg; while (*trimmed == ' ') trimmed++;
      int cy = startY + lineIdx * lineH;
      
      int tw = strlen(trimmed) * 12; // Size 2 width
      int cx = (SCREEN_W - tw) / 2;
      if (cx < 0) cx = 0;
      
      display->setTextSize(2);
      display->setCursor(cx+1, cy); display->print(trimmed); // Bold effect
      display->setCursor(cx, cy); display->print(trimmed);
      start = end; lineIdx++;
    }
  }

  if (strlen(lyricNext) > 0) {
    display->setTextColor(COLOR_FG);
    printCenter(lyricNext, 180, 1);
  }
}

bool SpotifyRemoteClass::update(int btnAction, Adafruit_GFX* display) {
  unsigned long now = millis();

  // Button logic
  if (btnAction == 1) { // Single click
    if (currentView == VIEW_PLAYER) {
      if (selectedCtrl == CTRL_BACK) selectedCtrl = CTRL_MENU;
      else if (selectedCtrl == CTRL_MENU) selectedCtrl = CTRL_PREV;
      else if (selectedCtrl == CTRL_PREV) selectedCtrl = CTRL_PAUSE;
      else if (selectedCtrl == CTRL_PAUSE) selectedCtrl = CTRL_NEXT;
      else selectedCtrl = CTRL_BACK;
    }
  } else if (btnAction == 2) { // Double click
    if (currentView == VIEW_LYRICS) {
      currentView = VIEW_PLAYER;
      selectedCtrl = CTRL_PAUSE;
    } else {
      if (selectedCtrl == CTRL_MENU) currentView = VIEW_LYRICS;
      else if (selectedCtrl != CTRL_BACK) sendControlCommand(selectedCtrl);
    }
  }

  // Animation & Timer logic
  if (abs(animPosition - (float)localPosition) > 0.01) {
    animPosition += ((float)localPosition - animPosition) * lerpSpeed;
  } else {
    animPosition = (float)localPosition;
  }

  // Lyric player animation logic (Push-up transition)
  if (strcmp(currentLyric, lyricActive) != 0) {
      strncpy(outgoingLyric, currentLyric, 63);
      strncpy(currentLyric, lyricActive, 63);
      currentLyric[63] = '\0';
      lyricAnimY = 15.0f; // Start 15 pixels below
  }
  
  if (lyricAnimY > 0.0f) {
      lyricAnimY -= 1.0f; // Fast Slide up (1.0 px per frame)
      if (lyricAnimY < 0.0f) lyricAnimY = 0.0f;
  }

  if (isPlaying && (now - lastPosUpdate >= 1000)) {
    lastPosUpdate = now;
    if (localPosition < songDuration) localPosition++;
  }

  if (titlePixelW > SCREEN_W - 10) {
    if (now - lastScrollTime >= 50) {
      lastScrollTime = now; scrollX--;
      if (scrollX < -(titlePixelW + 5)) scrollX = SCREEN_W - 15;
    }
  }

  static unsigned long lastDraw = 0;
  if (now - lastDraw >= 16) {
    lastDraw = now;
    if (currentView == VIEW_LYRICS) drawLyricsUI(display);
    else drawPlayerUI(display);
    return true;
  }
  return false;
}

SpotifyRemoteClass SpotifyRemote;

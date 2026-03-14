  #include <Update.h>
  #include <Arduino.h>
  #include <SPI.h>
  #include <Wire.h>
  #include <LittleFS.h>
  #include <WiFi.h>
  #include <DNSServer.h>      // NEW: Captive portal DNS server
  #include <WebServer.h>      // NEW: WebServer for Web UI
  #include <Adafruit_GFX.h>
  #include <Adafruit_GC9A01A.h>
  #include <Fonts/FreeSansBold12pt7b.h>
  #include "ntp_time.h"
  #include <esp_heap_caps.h>  // For PSRAM
  #include <esp_wifi.h>       // For low-level WiFi control (WPA2 force)
  #include <ESPmDNS.h>
  #include "SpotifyRemote.h"

  // Define ps_malloc as heap_caps_malloc for SPIRAM
  #define ps_malloc(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
  
  #define QOI_MALLOC(sz) ps_malloc(sz)
  #define QOI_FREE(p)    free(p)
  #define QOI_IMPLEMENTATION
  #include "qoi.h"

  #define TFT_CS   D0
  #define TFT_DC   D1
  #define TFT_RST  D2
  #define TFT_MOSI D10
  #define TFT_SCLK D8

  // The 240x240 main hardware display object
  Adafruit_GC9A01A display(TFT_CS, TFT_DC, TFT_RST);

  // GFX compatibility layer: A 128x64 virtual memory display that behaves precisely like the old SSD1306
  #define SCREEN_WIDTH 128
  #define SCREEN_HEIGHT 64
  GFXcanvas1 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  
  // A static buffer to drastically speed up 1-bit to 16-bit conversions (16KB RAM)
  static uint16_t canvasColors[SCREEN_WIDTH * SCREEN_HEIGHT];

  // Custom function to push the 128x64 canvas content centered onto the 240x240 screen extremely fast
  void canvasDisplay() {
      uint8_t *buffer = canvas.getBuffer();
      int idx = 0;
      for (int y = 0; y < SCREEN_HEIGHT; y++) {
          for (int x = 0; x < SCREEN_WIDTH; x++) {
              bool pixel = buffer[y * (SCREEN_WIDTH / 8) + (x / 8)] & (0x80 >> (x & 7));
              canvasColors[idx++] = pixel ? GC9A01A_WHITE : GC9A01A_BLACK;
          }
      }
      display.drawRGBBitmap((240 - SCREEN_WIDTH) / 2, (240 - SCREEN_HEIGHT) / 2, canvasColors, SCREEN_WIDTH, SCREEN_HEIGHT);
  }

  // --- WEB Setup ---
  WebServer server(80);
  DNSServer dnsServer;
  bool isWebUploadMode = false;

  // Touch Sensor pin (XIAO ESP32S3 - D3 pin)
  #define TOUCH_PIN D3
  #define TOUCH_ACTIVE_STATE HIGH // Ganti ke LOW jika sensor aktif saat disentuh ke gnd

  String folderList[20];
  int folderCount = 0;

  int menuState = 0; // 0=Main Menu, 1=Folder, 2=FPS, 3=WiFi
  int menuIndex = 0;
  
  String mainMenuList[] = {"Jam Realtime", "Sytm Info", "Tampilkan Media", "Wifi Scan", "Up Media", "Spotify Remote", "Reset System"};
  int mainMenuCount = 7;

  String wifiList[20];
  int wifiCount = 0;

  int fpsMenuIndex = 0;
  const uint8_t  CONTRAST_FULL  = 255;

  // --- PSRAM BINARY CACHING ---
  #define MAX_PSRAM_FRAMES 100
  uint16_t* psramFrames[MAX_PSRAM_FRAMES];
  int totalLoadedFrames = 0;
  String currentlyLoadedFolder = "";

  void clearPSRAMCache() {
      for (int i = 0; i < MAX_PSRAM_FRAMES; i++) {
          if (psramFrames[i] != NULL) {
              free(psramFrames[i]);
              psramFrames[i] = NULL;
          }
      }
      totalLoadedFrames = 0;
      currentlyLoadedFolder = "";
      Serial.println("PSRAM: Cache Cleared");
  }

  // Variabel untuk deteksi Double-Click
  uint32_t lastClickTime = 0;
  bool buttonState = !TOUCH_ACTIVE_STATE;
  bool lastButtonState = !TOUCH_ACTIVE_STATE;
  const uint32_t DOUBLE_CLICK_GAP = 250; // Maksimal 250ms antar klik
  const uint32_t DEBOUNCE_DELAY = 50;
  uint32_t lastDebounceTime = 0;

  // GIF FPS override yang dipilih user: 15, 30, atau 60
  int gifFps = 30; // default

  // Deteksi tombol tahan >= 500ms untuk exit GIF
  // Return true sekali saat threshold terpenuhi
  uint32_t btnHoldStart = 0;
  bool btnHoldFired = false;
  bool readButtonHeld() {
      bool pressed = (digitalRead(TOUCH_PIN) == TOUCH_ACTIVE_STATE);
      if (pressed) {
          if (btnHoldStart == 0) {
              btnHoldStart = millis();
              btnHoldFired = false;
          } else if (!btnHoldFired && (millis() - btnHoldStart >= 500)) {
              btnHoldFired = true;
              btnHoldStart = 0;
              return true;
          }
      } else {
          btnHoldStart = 0;
          btnHoldFired = false;
      }
      return false;
  }

  // Fungsi membaca klik (0 = Ga ngapa-ngapain, 1 = Single Click (NEXT), 2 = Double Click (ENTER))
  int readButtonState() {
      int action = 0;
      bool reading = digitalRead(TOUCH_PIN);
      uint32_t now = millis();

      // Debounce: kalau terjadi perubahan state fisik
      if (reading != lastButtonState) {
          lastDebounceTime = now;
      }

      // Validasi state tombol setelah melewati masa debounce
      if ((now - lastDebounceTime) > DEBOUNCE_DELAY) {
          if (reading != buttonState) {
              buttonState = reading;
              
              // Jika tombol baru saja DITEKAN
              if (buttonState == TOUCH_ACTIVE_STATE) {
                  if (now - lastClickTime < DOUBLE_CLICK_GAP) {
                      // Jarak waktu dengan klik sebelumnya masih masuk: DOUBLE CLICK!
                      action = 2;
                      lastClickTime = 0; // reset
                  } else {
                      // Ini baru klik pertama
                      lastClickTime = now;
                  }
              }
          }
      }

      // Cek kalau waktu gap habis dan gak ada klik kedua -> SINGLE CLICK!
      if (lastClickTime > 0 && (now - lastClickTime) > DOUBLE_CLICK_GAP) {
          // Hanya daftar jika saat ini tombol sudah dilepas atau sengaja ditahan
          if (buttonState != TOUCH_ACTIVE_STATE) {
              action = 1;
              lastClickTime = 0;
          }
      }

      lastButtonState = reading;
      return action;
  }

  void setContrast(uint8_t level) {
    // GC9A01A does not support simple contrast via software. Do nothing.
  }

  void listFolders() { // SCAN MEDIA: .bin file + BIN folder
    folderCount = 0;
    folderList[folderCount++] = "[ Back ]";
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file && folderCount < 20) {
      if (file.isDirectory()) {
         String dirName = String(file.name());
         bool hasMedia = false;
         File subDir = LittleFS.open("/" + dirName);
         if (subDir) {
             File subFile = subDir.openNextFile();
             while (subFile) {
                 String sfname = String(subFile.name());
                 sfname.toLowerCase();
                 if (sfname.startsWith("frame") && (sfname.endsWith(".bin") || sfname.endsWith(".qoi"))) {
                     hasMedia = true; break;
                 }
                 subFile = subDir.openNextFile();
             }
             subDir.close();
         }
         if (hasMedia) {
             folderList[folderCount++] = dirName + "/";
         }
      } else {
         String fname = String(file.name());
         String fl = fname; fl.toLowerCase();
         if (fl.endsWith(".bin") || fl.endsWith(".qoi")) {
             folderList[folderCount++] = fname;
         }
      }
      file = root.openNextFile();
    }
    root.close();
    if (folderCount == 1) {
      folderList[1] = "(No Media)";
      folderCount = 2;
    }
  }

  bool decodeQOI(File& f, uint16_t* outBuf) {
      // Clear outBuf immediately to black. If decode fails or dimensions are smaller, background stays black instead of TV static memory garbage.
      memset(outBuf, 0, 115200);

      size_t fSize = f.size();
      if (fSize == 0) return false;

      // Allocate temporary buffer for compressed QOI data in PSRAM
      void* qoiData = ps_malloc(fSize);
      if (!qoiData) {
          Serial.println("QOI: Failed to allocate memory for compressed data.");
          return false;
      }

      f.read((uint8_t*)qoiData, fSize);

      qoi_desc desc;
      // This will now use ps_malloc via our macro, safely allocating 230KB in PSRAM
      void* decodedPixels = qoi_decode(qoiData, fSize, &desc, 4); // Force RGBA (4 channels)
      free(qoiData);

      if (!decodedPixels) {
          Serial.println("QOI: qoi_decode failed to allocate uncompressed RGBA buffer.");
          return false;
      }

      // Convert from 32-bit RGBA to 16-bit RGB565 with byte-swapping for Adafruit SPI
      // Handle any QOI dimensions — center on 240x240 display, crop if larger
      uint8_t* rgba = (uint8_t*)decodedPixels;
      uint32_t srcW = desc.width;
      uint32_t srcH = desc.height;
      const uint32_t dstW = 240;
      const uint32_t dstH = 240;

      // Source offset (center-crop if source > display)
      uint32_t srcOffX = (srcW > dstW) ? (srcW - dstW) / 2 : 0;
      uint32_t srcOffY = (srcH > dstH) ? (srcH - dstH) / 2 : 0;
      // Destination offset (center if source < display)
      uint32_t dstOffX = (srcW < dstW) ? (dstW - srcW) / 2 : 0;
      uint32_t dstOffY = (srcH < dstH) ? (dstH - srcH) / 2 : 0;
      // Copy region
      uint32_t copyW = (srcW < dstW) ? srcW : dstW;
      uint32_t copyH = (srcH < dstH) ? srcH : dstH;

      for (uint32_t y = 0; y < copyH; y++) {
          for (uint32_t x = 0; x < copyW; x++) {
              uint32_t srcIdx = ((y + srcOffY) * srcW + (x + srcOffX)) * 4;
              uint32_t dstIdx = (y + dstOffY) * dstW + (x + dstOffX);
              uint8_t r = rgba[srcIdx + 0];
              uint8_t g = rgba[srcIdx + 1];
              uint8_t b = rgba[srcIdx + 2];
              uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
              outBuf[dstIdx] = color;
          }
      }

      free(decodedPixels);
      return true;
  }

  String getBinFormatName(size_t fSize) {
      if (fSize >= 230400) return "RGBA 32-bit";
      if (fSize >= 172800) return "RGB 24-bit";
      if (fSize >= 115200) return "RGB565 16-bit";
      if (fSize >= 7200)   return "1-bit Mono";
      return "Unknown Format";
  }

  void showBinFile(String filename) {
      String path = filename.startsWith("/") ? filename : "/" + filename;

      File f = LittleFS.open(path, "r");
      if (!f) { Serial.println("MEDIA: not found: " + path); return; }
      
      size_t fSize = f.size();
      bool isQoi = path.endsWith(".qoi") || path.endsWith(".QOI");
      Serial.printf("MEDIA: File size: %d bytes\n", fSize);
      String fmt = isQoi ? "QOI Image" : getBinFormatName(fSize);
      Serial.println("MEDIA: Detected Format: " + fmt);

      // Show format on OLED briefy
      canvas.fillScreen(0);
      canvas.setCursor(10, 20);
      canvas.println("Playing MEDIA:");
      canvas.setCursor(10, 35);
      canvas.println(fmt);
      canvasDisplay();
      delay(800);

      uint8_t header[4];
      if (!isQoi) {
          f.read(header, 4);
          f.seek(0); // Reset file pointer
          Serial.printf("MEDIA: First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", header[0], header[1], header[2], header[3]);
      }

      uint16_t* imgBuf = (uint16_t*)ps_malloc(115200); // 240x240x2
      if (!imgBuf) { Serial.println("MEDIA: OOM!"); f.close(); return; }

      if (isQoi) {
          decodeQOI(f, imgBuf);
      } else if (fSize >= 230400) { // 32-bit RGBA
          if (fSize > 230400) f.seek(fSize - 230400); // Skip header
          uint8_t r_buf[240*4];
          for (int y = 0; y < 240; y++) {
              f.read(r_buf, 240*4);
              for (int x = 0; x < 240; x++) {
                  uint8_t r = r_buf[x*4+0], g = r_buf[x*4+1], b = r_buf[x*4+2];
                  uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                  imgBuf[y*240+x] = color; 
              }
          }
      } else if (fSize >= 172800) { // 24-bit RGB
          if (fSize > 172800) f.seek(fSize - 172800); // Skip header
          uint8_t r_buf[240*3];
          for (int y = 0; y < 240; y++) {
              f.read(r_buf, 240*3);
              for (int x = 0; x < 240; x++) {
                  uint8_t r = r_buf[x*3+0], g = r_buf[x*3+1], b = r_buf[x*3+2];
                  uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                  imgBuf[y*240+x] = color; 
              }
          }
      } else if (fSize >= 7200 && fSize < 10000) { 
          // 1-bit Monochrome (240x240 / 8 = 7200 bytes)
          if (fSize > 7200) f.seek(fSize - 7200);
          uint8_t* bits = (uint8_t*)malloc(7200);
          if (bits) {
              f.read(bits, 7200);
              for (int i = 0; i < 57600; i++) {
                  int byteIdx = i / 8;
                  int bitIdx = 7 - (i % 8); 
                  bool pixel = (bits[byteIdx] >> bitIdx) & 0x01;
                  imgBuf[i] = pixel ? 0xFFFF : 0x0000;
              }
              free(bits);
          }
      } else {
          // If source is already 16-bit (115200 bytes), skip potential header
          if (fSize > 115200) f.seek(fSize - 115200);
          f.read((uint8_t*)imgBuf, 115200);
          
          for(int p=0; p<57600; p++) {
              uint16_t c = imgBuf[p];
              imgBuf[p] = (c >> 8) | (c << 8);
          }
      }
      f.close();

      display.drawRGBBitmap(0, 0, imgBuf, 240, 240);
      free(imgBuf);

      Serial.println("MEDIA: done. Tahan BOOT 500ms = kembali.");
      btnHoldStart = 0; btnHoldFired = false;
      while (true) { delay(10); if (readButtonHeld()) break; }
      display.fillScreen(GC9A01A_BLACK);
  }

  void playBinFrames(String selection) {
      String folder = selection;
      if (folder.endsWith("/")) folder.remove(folder.length()-1);
      String fullPath = "/" + folder;

      if (currentlyLoadedFolder != fullPath) {
          clearPSRAMCache();
          canvas.fillScreen(0);
          canvas.setCursor(10, 20);
          canvas.println("Loading PSRAM...");
          canvas.setCursor(10, 30);
          canvas.println(folder);
          canvasDisplay();

          String seqExt = "";
          int frameCount = 0;
          int firstFrameIdx = -1;
          File dir = LittleFS.open(fullPath);
          if (dir) {
              File entry = dir.openNextFile();
              while (entry) {
                  String ename = String(entry.name());
                  String elow = ename; elow.toLowerCase();
                  if (elow.startsWith("frame") && (elow.endsWith(".bin") || elow.endsWith(".qoi"))) {
                      frameCount++;
                      int idx = elow.substring(5, 9).toInt();
                      if (firstFrameIdx == -1 || idx < firstFrameIdx) {
                          firstFrameIdx = idx;
                          seqExt = ename.substring(ename.lastIndexOf('.')); // Keep exact casing of the first found file
                      }
                  }
                  entry = dir.openNextFile();
              }
              dir.close();
          }

          if (frameCount > MAX_PSRAM_FRAMES) frameCount = MAX_PSRAM_FRAMES;
          bool isQoiSeq = seqExt.equalsIgnoreCase(".qoi");

          if (firstFrameIdx != -1) {
              for (int i = 0; i < frameCount; i++) {
                  if (seqExt == "") break;
                  char frameName[32];
                  sprintf(frameName, "/frame%04d", firstFrameIdx + i);
                  String framePath = fullPath + String(frameName) + seqExt;
                  
                  File f = LittleFS.open(framePath, "r");
                  if (!f) continue;

              size_t fSize = f.size();
              // Hide redundant logs for frames
              
              // Safeguard: Check if PSRAM has enough space (need ~115KB)
              size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
              if (freePSRAM < 131072) { // Less than 128KB free
                  Serial.println("PSRAM: Almost full, stopping load.");
                  f.close();
                  break;
              }

              if (i == 0) {
                  String fmt = isQoiSeq ? "QOI Anim" : getBinFormatName(fSize);
                  Serial.println("PSRAM: Anim Format: " + fmt);
                  canvas.setCursor(10, 45);
                  canvas.print("Fmt: "); canvas.println(fmt);
                  canvasDisplay();
              }
              
              uint8_t header[4];
              if (!isQoiSeq) {
                  f.read(header, 4);
                  f.seek(0); // Reset file pointer
                  Serial.printf("PSRAM: Frame %d first 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", i, header[0], header[1], header[2], header[3]);
              }

              psramFrames[i] = (uint16_t*)ps_malloc(115200);
              if (psramFrames[i] == NULL) { Serial.println("PSRAM: OOM!"); f.close(); break; }

              if (isQoiSeq) {
                  decodeQOI(f, psramFrames[i]);
              } else if (fSize >= 230400) { // 32-bit RGBA
                  if (fSize > 230400) f.seek(fSize - 230400);
                  uint8_t r_buf[240*4];
                  for (int y = 0; y < 240; y++) {
                      f.read(r_buf, 240*4);
                      for (int x = 0; x < 240; x++) {
                          uint8_t r = r_buf[x*4+0], g = r_buf[x*4+1], b = r_buf[x*4+2];
                          uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                          psramFrames[i][y*240+x] = color;
                      }
                  }
              } else if (fSize >= 172800) { // 24-bit RGB
                  if (fSize > 172800) f.seek(fSize - 172800);
                  uint8_t r_buf[240*3];
                  for (int y = 0; y < 240; y++) {
                      f.read(r_buf, 240*3);
                      for (int x = 0; x < 240; x++) {
                          uint8_t r = r_buf[x*3+0], g = r_buf[x*3+1], b = r_buf[x*3+2];
                          uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                          psramFrames[i][y*240+x] = color;
                      }
                  }
              } else if (fSize >= 7200 && fSize < 10000) {
                  // 1-bit Monochrome
                  if (fSize > 7200) f.seek(fSize - 7200);
                  uint8_t* bits = (uint8_t*)malloc(7200);
                  if (bits) {
                      f.read(bits, 7200);
                      for (int j = 0; j < 57600; j++) {
                          int byteIdx = j / 8;
                          int bitIdx = 7 - (j % 8);
                          bool pixel = (bits[byteIdx] >> bitIdx) & 0x01;
                          psramFrames[i][j] = pixel ? 0xFFFF : 0x0000;
                      }
                      free(bits);
                  }
              } else {
                  // If source is already 16-bit (115200 bytes), skip potential header
                  if (fSize > 115200) f.seek(fSize - 115200);
                  f.read((uint8_t*)psramFrames[i], 115200);
                  for(int p=0; p<57600; p++) {
                      uint16_t c = psramFrames[i][p];
                      psramFrames[i][p] = (c >> 8) | (c << 8);
                  }
              }
              f.close();
              totalLoadedFrames++;
          }
          }
          currentlyLoadedFolder = fullPath;
      }

      // FPS dari pilihan user
      int32_t binDelayMs = 1000 / gifFps;
      Serial.printf("MEDIA: Playing at %dfps (interval=%dms)\n", gifFps, binDelayMs);

      // Reset hold detector
      btnHoldStart = 0;
      btnHoldFired = false;

      bool exitBin = false;
      int currentFrame = 0;
      uint32_t nextFrameTime = millis();
      
      while (!exitBin) {
          if (totalLoadedFrames > 0) {
              display.drawRGBBitmap(0, 0, psramFrames[currentFrame], 240, 240);
              currentFrame = (currentFrame + 1) % totalLoadedFrames;
          }
          
          nextFrameTime += binDelayMs;
          
          // Always check button at least once per frame, even if draw time is long
          if (readButtonHeld()) exitBin = true;
          
          // Precise jitter-free wait using absolute future time
          while (!exitBin && (millis() < nextFrameTime)) {
              yield(); // Let system headers run
              if (readButtonHeld()) {
                  exitBin = true;
                  break;
              }
          }
          
          // If we fell way behind, resync the clock to prevent fast-forwarding
          if (millis() > nextFrameTime + binDelayMs) {
              nextFrameTime = millis();
          }
      }
      display.fillScreen(GC9A01A_BLACK);
  }

  void drawMenu() {
    canvas.fillScreen(0); // clear virtual buffer
    canvas.setTextSize(1);
    canvas.setTextColor(1); // WHITE for monochrome canvas

    if (menuState == 0) {
      String title = "=== MAIN MENU ===";
      int16_t x1, y1; uint16_t w, h;
      canvas.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
      canvas.setCursor((SCREEN_WIDTH - w)/2, 0);
      canvas.println(title);
      
      int startObj = menuIndex - 4;
      if (startObj < 0) startObj = 0;
      for (int i = startObj, j = 0; i < mainMenuCount && j < 5; i++, j++) {
        canvas.setCursor(0, (j + 1) * 10 + 5);
        if (i == menuIndex) canvas.print("> ");
        else canvas.print("  ");
        canvas.print(mainMenuList[i]);
      }
    } else if (menuState == 1) {
      if (folderCount == 0 || (folderCount == 2 && folderList[1] == "(No .bin Media)")) {
        canvas.setCursor(0, 20);
        canvas.println("NO MEDIA FOUND!");
        canvas.println("Upload .bin files");
        canvas.setCursor(0, 50);
        canvas.println("> Back (2x Click)");
      } else {
        int startObj = menuIndex - 4;
        if (startObj < 0) startObj = 0;
        for (int i = startObj, j = 0; i < folderCount && j < 5; i++, j++) {
          canvas.setCursor(0, (j + 1) * 10 + 5);
          if (i == menuIndex) canvas.print("> ");
          else canvas.print("  ");
          String s = folderList[i];
          if (s.length() > 18) s = s.substring(0, 18);
          canvas.print(s);
        }
      }
    } else if (menuState == 2) {
      // FPS Menu
      String title2 = "=== PILIH FPS ===";
      int16_t x1, y1; uint16_t w, h;
      canvas.getTextBounds(title2, 0, 0, &x1, &y1, &w, &h);
      canvas.setCursor((SCREEN_WIDTH - w)/2, 0);
      canvas.println(title2);
      const char* fpsOpts[] = {"15 FPS", "30 FPS", "60 FPS"};
      for (int i = 0; i < 3; i++) {
        canvas.setCursor(0, (i + 1) * 12 + 8);
        if (i == menuIndex) canvas.print("> ");
        else canvas.print("  ");
        canvas.print(fpsOpts[i]);
        int thisFps = (i == 0) ? 15 : (i == 1) ? 30 : 60;
        if (thisFps == gifFps) canvas.print(" *");
      }
    } else if (menuState == 3) {
      String title = "=== PILIH WIFI ===";
      int16_t x1, y1; uint16_t w, h;
      canvas.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
      canvas.setCursor((SCREEN_WIDTH - w)/2, 0);
      canvas.println(title);
      
      int startObj = menuIndex - 4;
      if (startObj < 0) startObj = 0;
      for (int i = startObj, j = 0; i < wifiCount && j < 5; i++, j++) {
        canvas.setCursor(0, (j + 1) * 10 + 5);
        if (i == menuIndex) canvas.print("> ");
        else canvas.print("  ");
        String s = wifiList[i];
        if (s.length() > 18) s = s.substring(0, 18);
        canvas.print(s);
      }
    }
    
    canvasDisplay();
  }

  void showSystemInfo(bool waitForKey) {
    float totalRAM = ESP.getHeapSize() / (1024.0 * 1024.0);
    float usedRAM  = (ESP.getHeapSize() - ESP.getFreeHeap()) / (1024.0 * 1024.0);
    int pctRAM = (ESP.getHeapSize() > 0) ? (int)((usedRAM / totalRAM) * 100) : 0;

    float totalPSRAM = ESP.getPsramSize() / (1024.0 * 1024.0);
    float usedPSRAM  = (ESP.getPsramSize() - ESP.getFreePsram()) / (1024.0 * 1024.0);
    int pctPSRAM = (ESP.getPsramSize() > 0) ? (int)((usedPSRAM / totalPSRAM) * 100) : 0;

    float totalFS = LittleFS.totalBytes() / (1024.0 * 1024.0);
    float usedFS = LittleFS.usedBytes() / (1024.0 * 1024.0);
    int pctFS = (LittleFS.totalBytes() > 0) ? (int)((usedFS / totalFS) * 100) : 0;

    canvas.fillScreen(0);
    canvas.setTextSize(1);
    canvas.setTextColor(1);
    
    String title = "=== SYSTEM INFO ===";
    int16_t x1, y1; uint16_t w, h;
    canvas.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
    canvas.setCursor((SCREEN_WIDTH - w)/2, 0);
    canvas.println(title);

    canvas.setCursor(0, 15);
    canvas.printf("FS :%.1fM|%.1fM(%d%%)\n", totalFS, usedFS, pctFS);
    canvas.printf("RAM:%.2fM|%.2fM(%d%%)\n", totalRAM, usedRAM, pctRAM);
    
    if (totalPSRAM > 0) {
      canvas.printf("PSR:%.1fM|%.1fM(%d%%)\n", totalPSRAM, usedPSRAM, pctPSRAM);
    } else {
      canvas.println("PSR: Not Detected");
    }

    if (waitForKey) {
        canvas.setCursor(0, 50);
        canvas.println("> Press Any Btn <");
    } else {
        canvas.setCursor(0, 50);
        canvas.println("Booting System...");
    }
    
    canvasDisplay();

    if (waitForKey) {
        delay(300); // debounce
        while (true) {
            int btn = readButtonState();
            if (btn == 2) break;
            yield();
        }
    }
  }
// =====================================================================
// REAL-TIME CLOCK UI
// =====================================================================

struct ClockColor {
    uint8_t r, g, b; // 5-6-5 components for blending
    uint16_t val;    // Full RGB565 value
};

static const ClockColor clockPalette[] = {
    {0, 63, 31,  0x07FF}, // Cyan
    {31, 0, 31,  0xF81F}, // Magenta
    {31, 63, 0,  0xFFE0}, // Yellow
    {0, 63, 0,   0x07E0}, // Green
    {31, 31, 0,  0xFD20}, // Orange
    {31, 0, 15,  0xF810}, // Pink
    {31, 63, 31, 0xFFFF}  // White
};
static int currentPaletteIndex = 0;

// Ease Out Cubic function for smooth snapping
float easeOutCubic(float t) {
    return 1.0f - pow(1.0f - t, 3.0f);
}

// Helper to draw a filled heart with custom graphics (since fonts lack them)
void drawAAFilledHeart(int x, int y, int size, uint16_t color) {
    // A simple beautiful heart shape using primitive shapes
    int r = size / 4;
    display.fillCircle(x - r, y - r, r, color);
    display.fillCircle(x + r, y - r, r, color);
    display.fillTriangle(x - 2 * r, y - r/2, x + 2 * r, y - r/2, x, y + 2 * r, color);
}

// Shared buffer to avoid frequent malloc/free causing flicker
static uint16_t aaLineBuf[240];

void drawAARingSegment(int x1, int x2, int y, float r_in, float r_out, float targetAngle, uint8_t tr, uint8_t tg, uint8_t tb, uint16_t tval) {
    if (x1 < 0) x1 = 0; if (x2 > 239) x2 = 239;
    int w = x2 - x1 + 1;
    if (w <= 0) return;
    
    float dy = y - 120.0f;
    for (int i = 0; i < w; i++) {
        float dx = (x1 + i) - 120.0f;
        float distSq = dx*dx + dy*dy;
        float dist = sqrt(distSq);
        
        float alphaDist = 1.0f;
        if (dist < r_in) alphaDist = dist - (r_in - 1.0f);
        else if (dist > r_out) alphaDist = (r_out + 1.0f) - dist;
        if (alphaDist < 0.0f) alphaDist = 0.0f;
        if (alphaDist > 1.0f) alphaDist = 1.0f;
        
        if (targetAngle < 360.0f) {
            float angleRad = atan2(dy, dx);
            float angleDeg = angleRad * 180.0f / PI + 90.0f;
            if (angleDeg < 0.0f) angleDeg += 360.0f;
            
            float alphaAng = 1.0f;
            if (targetAngle <= 0.0f) alphaAng = 0.0f;
            else if (angleDeg > targetAngle) {
                alphaAng = 1.0f - (angleDeg - targetAngle);
                if (alphaAng < 0.0f) alphaAng = 0.0f;
            }
            if (angleDeg > 359.0f) {
                float dStart = 360.0f - angleDeg; 
                if (dStart < 1.0f && dStart < alphaAng) alphaAng = dStart;
            }
            alphaDist *= alphaAng;
        }

        if (alphaDist <= 0.0f) {
            aaLineBuf[i] = GC9A01A_BLACK;
        } else if (alphaDist >= 1.0f) {
            aaLineBuf[i] = tval;
        } else {
            uint16_t r = (uint16_t)(tr * alphaDist);
            uint16_t g = (uint16_t)(tg * alphaDist);
            uint16_t b = (uint16_t)(tb * alphaDist);
            aaLineBuf[i] = (r << 11) | (g << 5) | b;
        }
    }
    display.drawRGBBitmap(x1, y, aaLineBuf, w, 1);
}

void drawAAFullRing(float r_in, float r_out, uint8_t r, uint8_t g, uint8_t b, uint16_t val) {
    for (int y = 0; y < 240; y++) {
        float dy = y - 120.0f;
        float out_sq = (r_out+1.0f)*(r_out+1.0f) - dy*dy;
        if (out_sq < 0.0f) continue;
        float dx_out = sqrt(out_sq);
        
        float in_sq = (r_in-1.0f)*(r_in-1.0f) - dy*dy;
        if (in_sq > 0.0f) {
            float dx_in = sqrt(in_sq);
            int x1 = 120 - (int)ceil(dx_out);
            int x2 = 120 - (int)floor(dx_in);
            drawAARingSegment(x1, x2, y, r_in, r_out, 360.0f, r, g, b, val);
            
            int x3 = 120 + (int)floor(dx_in);
            int x4 = 120 + (int)ceil(dx_out);
            drawAARingSegment(x3, x4, y, r_in, r_out, 360.0f, r, g, b, val);
        } else {
            int x_start = 120 - (int)ceil(dx_out);
            int x_end = 120 + (int)ceil(dx_out);
            drawAARingSegment(x_start, x_end, y, r_in, r_out, 360.0f, r, g, b, val);
        }
    }
}

// Draws only the arc segment from angleFrom to angleTo (no clear, incremental)
void drawAARingArc(float angleFrom, float angleTo, float r_in, float r_out, const ClockColor& col) {
    if (angleFrom >= angleTo) return;
    for (int y = 0; y < 240; y++) {
        float dy = y - 120.0f;
        float out_sq = (r_out + 1.0f)*(r_out + 1.0f) - dy*dy;
        if (out_sq < 0.0f) continue;
        float dx_out = sqrtf(out_sq);
        float in_sq  = (r_in  - 1.0f)*(r_in  - 1.0f) - dy*dy;
        if (in_sq > 0.0f) {
            float dx_in = sqrtf(in_sq);
            drawAARingSegment(120-(int)ceilf(dx_out), 120-(int)floorf(dx_in), y, r_in, r_out, angleTo, col.r, col.g, col.b, col.val);
            drawAARingSegment(120+(int)floorf(dx_in), 120+(int)ceilf(dx_out), y, r_in, r_out, angleTo, col.r, col.g, col.b, col.val);
        } else {
            drawAARingSegment(120-(int)ceilf(dx_out), 120+(int)ceilf(dx_out), y, r_in, r_out, angleTo, col.r, col.g, col.b, col.val);
        }
    }
}

void runRealtimeClock() {
    display.fillScreen(GC9A01A_BLACK);

    // Ring geometry (borders)
    const float R    = 118.5f;
    const float W    = 5.0f;
    const float r_in  = R - W/2.0f;  // 116.0
    const float r_out = R + W/2.0f;  // 121.0

    // Draw white inner ring ONCE — never cleared
    drawAAFullRing(107.0f, 111.0f, 31, 63, 31, GC9A01A_WHITE);
    // Clear the colored border ring area once on entry
    drawAAFullRing(r_in - 1.0f, r_out + 1.0f, 0, 0, 0, GC9A01A_BLACK);

    // --- BACKGROUND WIFI & NTP (only in clock mode) ---
    // ntpEverSynced and lastNtpSyncMs are static so they persist across clock re-entries
    static bool ntpEverSynced = false;
    static uint32_t lastNtpSyncMs = 0;
    static uint32_t lastWifiBegin = 0;
    static bool clockWifiActive = false;

    struct tm t_init;
    bool initHasTime = getLocalTime(&t_init, 0);
    uint32_t now_entry = millis();
    bool needResync = !ntpEverSynced || (now_entry - lastNtpSyncMs > 3600000UL); // resync after 1 hour

    if (needResync && !clockWifiActive) {
        clockWifiActive = true;
        WiFi.persistent(true);
        WiFi.setAutoReconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        WiFi.begin("M.Jarez", "samsito140671");
        lastWifiBegin = now_entry;
        Serial.printf("[Clock] Starting WiFi for NTP. Synced before: %d, last sync: %lums ago\n",
            ntpEverSynced, now_entry - lastNtpSyncMs);
    } else if (!needResync) {
        Serial.println("[Clock] NTP already synced recently, skipping WiFi.");
    }

    int lastSec = -1;
    bool lastHasTime = false;
    char lastTStr[12] = "";
    char lastDStr[32] = "";
    bool footerDrawn = false;
    uint32_t lastNtpAttempt = 0;
    
    while (true) {
        uint32_t now = millis();
        if (readButtonHeld()) break;

        // --- BACKGROUND LOGIC ---
        struct tm t;
        bool hasTime = getLocalTime(&t, 0);
        // Latch: once NTP is synced, never allow hasTime to go back to false
        // (brief false returns from getLocalTime after WiFi off are spurious)
        if (!hasTime && ntpEverSynced) {
            // Force a re-read with a small patience (2ms)
            hasTime = getLocalTime(&t, 2);
        }
        
        wl_status_t wfStatus = WiFi.status();

        // Retry every 20s if WiFi is active but not connected yet
        static uint32_t lastRetry = 0;
        if (clockWifiActive && !hasTime && wfStatus != WL_CONNECTED && (now - lastWifiBegin > 20000) && (now - lastRetry > 20000)) {
            lastRetry = now;
            Serial.println("[Clock] WiFi retry...");
            WiFi.disconnect();
            delay(200);
            esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
            WiFi.begin("M.Jarez", "samsito140671");
            lastWifiBegin = now;
        }
        
        // If WiFi connects, trigger NTP request
        if (clockWifiActive && !hasTime && wfStatus == WL_CONNECTED && (now - lastNtpAttempt > 5000)) {
            lastNtpAttempt = now;
            configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
            Serial.println("[Clock] WiFi Connected! Sending NTP request...");
        }
        
        // Once synced: record timestamp, stop WiFi to save power
        if (hasTime && !lastHasTime) {
            ntpEverSynced = true;
            lastNtpSyncMs = millis();
            clockWifiActive = false;
            if (WiFi.getMode() != WIFI_OFF) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                Serial.println("[Clock] NTP synced! WiFi OFF. Will resync after 1 hour.");
            }
        }

        int currentSec = hasTime ? t.tm_sec : -2; // -2 = NTP not ready, don't draw ring

        // Only update if something changed
        if (currentSec == lastSec && hasTime == lastHasTime) {
            yield();
            continue;
        }
        
        // When time is first synced, reset the ring to start fresh from second 0
        if (!lastHasTime && hasTime) {
            lastSec = -1;
            drawAAFullRing(r_in - 1.0f, r_out + 1.0f, 0, 0, 0, GC9A01A_BLACK);
        }
        lastHasTime = hasTime;

        // Only draw the ring if NTP time is available
        if (hasTime && currentSec >= 0) {
            if (currentSec == 0 && lastSec > 0) {
                // Minute changed: clear the entire ring and change color
                currentPaletteIndex = (currentPaletteIndex + 1) % (sizeof(clockPalette)/sizeof(clockPalette[0]));
                drawAAFullRing(r_in - 1.0f, r_out + 1.0f, 0, 0, 0, GC9A01A_BLACK);
            } else {
                // INCREMENTAL: draw arc from previous second to current second
                float fromAngle = (lastSec < 0 ? 0 : lastSec) * 6.0f;
                float toAngle   = currentSec * 6.0f;
                if (lastSec < 0 || toAngle < fromAngle) {
                    fromAngle = 0.0f;
                }
                const ClockColor& col = clockPalette[currentPaletteIndex];
                drawAARingArc(fromAngle, toAngle, r_in, r_out, col);
            }
            lastSec = currentSec;
        }

        // --- DRAW TIME ---
        char tStr[12];
        if (hasTime) {
            snprintf(tStr, sizeof(tStr), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        } else {
            snprintf(tStr, sizeof(tStr), "--:--:--");
        }
        if (strcmp(tStr, lastTStr) != 0) {
            display.setTextColor(GC9A01A_WHITE, GC9A01A_BLACK);
            display.setTextSize(3);
            int16_t tx1, ty1; uint16_t tw, th;
            display.getTextBounds(tStr, 0, 0, &tx1, &ty1, &tw, &th);
            display.setCursor((240 - tw) / 2, 85);
            display.print(tStr);
            strcpy(lastTStr, tStr);
        }
        
        // --- DRAW DATE ---
        const char * days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        const char * months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        char dStr[32];
        if (hasTime) {
            snprintf(dStr, sizeof(dStr), "%s, %02d %s %04d", days[t.tm_wday], t.tm_mday, months[t.tm_mon], t.tm_year + 1900);
        } else {
            if (wfStatus == WL_CONNECTED) {
                snprintf(dStr, sizeof(dStr), "SYNCING TIME...");
            } else if (wfStatus == WL_NO_SSID_AVAIL) {
                snprintf(dStr, sizeof(dStr), "SSID NOT FOUND");
            } else if (wfStatus == WL_CONNECT_FAILED) {
                snprintf(dStr, sizeof(dStr), "CONN FAILED");
            } else {
                snprintf(dStr, sizeof(dStr), "CONNECTING WIFI...");
            }
        }
        if (strcmp(dStr, lastDStr) != 0) {
            display.setTextColor(GC9A01A_CYAN, GC9A01A_BLACK);
            display.setTextSize(1);
            int16_t dx1, dy1; uint16_t dw, dh;
            display.getTextBounds(dStr, 0, 0, &dx1, &dy1, &dw, &dh);
            display.fillRect(30, 125, 180, 12, GC9A01A_BLACK);
            display.setCursor((240 - dw) / 2, 125);
            display.print(dStr);
            strcpy(lastDStr, dStr);
        }

        // --- DRAW FOOTER (once total) ---
        if (!footerDrawn) {
            footerDrawn = true;
            const char* name = "Alifia";
            display.setTextColor(0xF81F, GC9A01A_BLACK);
            display.setTextSize(1);
            int16_t nx1, ny1; uint16_t nw, nh;
            display.getTextBounds(name, 0, 0, &nx1, &ny1, &nw, &nh);
            int heartSize = 8;
            int totalW = nw + 6 + heartSize;
            int startX = (240 - totalW) / 2;
            display.setCursor(startX, 143);
            display.print(name);
            drawAAFilledHeart(startX + nw + 6 + heartSize/2, 147, heartSize, 0xF800);
        }

        yield();
    }
    // Clean up WiFi when exiting clock mode
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    clockWifiActive = false; // Turn off WiFi activity flag on exit
    neopixelWrite(48, 0, 0, 0);
}



  // No legacy RGB buffer needed

  // Legacy viewers removed

  // Legacy viewers and callbacks removed
  // Legacy GIF and Boot animation removed

  // ===================================
  // API & Handlers for Web Upload
  // ===================================

  void sendJsonResponse(int code, String json) {
      server.send(code, "application/json", json);
  }

  void handleStatus() {
      float totalFS = LittleFS.totalBytes() / (1024.0 * 1024.0);
      float usedFS = LittleFS.usedBytes() / (1024.0 * 1024.0);
      float freeFS = totalFS - usedFS;
      float totalRAM = ESP.getHeapSize() / 1024.0;
      float freeRAM = ESP.getFreeHeap() / 1024.0;
      
      String json = "{";
      json += "\"status\":\"ok\",";
      json += "\"fs_total_mb\":" + String(totalFS, 2) + ",";
      json += "\"fs_used_mb\":" + String(usedFS, 2) + ",";
      json += "\"fs_free_mb\":" + String(freeFS, 2) + ",";
      json += "\"ram_total_kb\":" + String(totalRAM, 2) + ",";
      json += "\"ram_free_kb\":" + String(freeRAM, 2);
      json += "}";
      sendJsonResponse(200, json);
  }

  void handleList() {
      String json = "{\"files\":[";
      File root = LittleFS.open("/");
      File file = root.openNextFile();
      bool first = true;
      while (file) {
          if (!first) json += ",";
          json += "{\"name\":\"" + String(file.name()) + "\",";
          json += "\"is_dir\":" + String(file.isDirectory() ? "true" : "false") + ",";
          json += "\"size\":" + String(file.size()) + "}";
          first = false;
          file = root.openNextFile();
      }
      json += "]}";
      sendJsonResponse(200, json);
  }

  void handleDelete() {
      if (!server.hasArg("path")) {
          sendJsonResponse(400, "{\"error\":\"missing path\"}");
          return;
      }
      String path = server.arg("path");
      if (!path.startsWith("/")) path = "/" + path;
      
      if (LittleFS.exists(path)) {
          if (LittleFS.remove(path) || LittleFS.rmdir(path)) {
              sendJsonResponse(200, "{\"status\":\"deleted\"}");
          } else {
              sendJsonResponse(500, "{\"error\":\"delete failed\"}");
          }
      } else {
          sendJsonResponse(404, "{\"error\":\"not found\"}");
      }
  }

  void handleFileUpload() {
      HTTPUpload& upload = server.upload();
      static File fsUploadFile;
      
      if (upload.status == UPLOAD_FILE_START) {
          String filename = upload.filename;
          if (!filename.startsWith("/")) filename = "/" + filename;
          
          // Create directories if they don't exist
          int lastSlash = filename.lastIndexOf('/');
          if (lastSlash > 0) {
              String dirPath = filename.substring(0, lastSlash);
              if (!LittleFS.exists(dirPath)) {
                  LittleFS.mkdir(dirPath);
              }
          }

          fsUploadFile = LittleFS.open(filename, FILE_WRITE);
          Serial.print("Upload Start: "); Serial.println(filename);

          canvas.fillScreen(0);
          canvas.setTextSize(1);
          canvas.setCursor(10, 20);
          canvas.println("WRITING...");
          canvas.setCursor(10, 30);
          String shortName = filename.length() > 20 ? "..." + filename.substring(filename.length()-17) : filename;
          canvas.println(shortName);
          canvasDisplay();
      } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (fsUploadFile) fsUploadFile.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END) {
          if (fsUploadFile) {
              fsUploadFile.close();
              Serial.print("Upload Size: "); Serial.println(upload.totalSize);
          }
          
          if (server.args() == 0) { // Check if this is the last part
             // We don't send response yet as there might be more files in the batch
          }
      }

  }

  void runMediaUploaderServer() {
      isWebUploadMode = true;
      
      // Disconnect from STA first to ensure clean AP start
      WiFi.disconnect(true);
      WiFi.mode(WIFI_AP);
      
      // Configure explicit IP to prevent DHCP issues on some phones
      IPAddress apIP(192, 168, 4, 1);
      WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
      
      // Start AP on channel 6
      WiFi.softAP("ESP32-Media-App", "12345678", 6);

      // Setup mDNS
      if (MDNS.begin("ganci")) {
          Serial.println("mDNS responder started: ganci.local");
      }

      server.on("/status", HTTP_GET, handleStatus);
      server.on("/list", HTTP_GET, handleList);
      server.on("/delete", HTTP_POST, handleDelete);
      server.on("/upload", HTTP_POST, []() {
          sendJsonResponse(200, "{\"status\":\"success\"}");
      }, handleFileUpload);
      
      server.on("/reboot", HTTP_POST, []() {
          sendJsonResponse(200, "{\"status\":\"rebooting\"}");
          delay(500);
          ESP.restart();
      });

      server.onNotFound([]() {
          sendJsonResponse(404, "{\"error\":\"not found\"}");
      });
      server.begin();

      // Show IP on screen
      String ip = WiFi.softAPIP().toString();
      
      while(true) {
          dnsServer.processNextRequest();
          server.handleClient();
          
          canvas.fillScreen(0);
          canvas.setTextSize(1);
          canvas.setCursor(0, 0);
          canvas.println("ANDROID API MODE");
          canvas.setCursor(0, 15);
          canvas.print("Wi-Fi: ");
          canvas.println("ESP32-Media-App");
          canvas.setCursor(0, 30);
          canvas.print("MDNS : "); canvas.println("ganci.local");
          canvas.setCursor(0, 45);
          canvas.print("IP   : "); canvas.print(ip);
          canvas.setCursor(0, 60);
          canvas.println("2x Klik = Batal");
          canvasDisplay();
          
          int btn = readButtonState();
          if (btn == 2) {
              // exit on double tab
              server.stop();
              WiFi.mode(WIFI_STA);
              isWebUploadMode = false;
              break; 
          }
          yield();
      }
  }

  // ===================================
  // MAIN SYSTEM
  // ===================================

void setup() {

    Serial.begin(115200);
    pinMode(TOUCH_PIN, INPUT);

    // LED D2 tidak digunakan
    pinMode(2, OUTPUT);
    digitalWrite(2, LOW);
    // NeoPixel dimatikan saat boot
    neopixelWrite(48, 0, 0, 0);

    // Initialize LittleFS
    if(!LittleFS.begin(true)){
        Serial.println("LittleFS Mount Failed");
    }

    // WiFi hanya konek saat diperlukan (Clock mode) - tidak konek saat boot
    WiFi.mode(WIFI_OFF);

    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); 
    display.begin(80000000); // 40MHz for Display
    setCpuFrequencyMhz(160); // Boost to 160MHz for better stability
    display.setRotation(0);
    display.fillScreen(GC9A01A_BLACK);

    setContrast(CONTRAST_FULL);

    listFolders();
    drawMenu();

    Serial.println("\n=== Gunakan BOOT button: 1x Klik = NEXT, 2x Klik = ENTER | Tahan 500ms = Exit GIF ===");
  }

  void loop() {
    // ============================================================
    // AUTO-SYNC NTP: Non-blocking 3-state machine
    //   State 0 = IDLE (tunggu interval)
    //   State 1 = CONNECTING (tunggu WiFi konek)
    //   State 2 = NTP_WAIT (tunggu NTP terset di RTC)
    // Tidak ada delay() atau while() — berjalan di setiap iterasi loop
    // ============================================================

    bool nextTriggered = false;
    bool enterTriggered = false;

    int btn = readButtonState();
    if (btn == 1) {
        nextTriggered = true;
    } else if (btn == 2) {
        enterTriggered = true;
    }

    if (nextTriggered) {
      if (menuState == 0) {
        menuIndex++;
        if (menuIndex >= mainMenuCount) menuIndex = 0;
      } else if (menuState == 1) {
        menuIndex++;
        if (menuIndex >= folderCount) menuIndex = 0;
      } else if (menuState == 2) {
        menuIndex++;
        if (menuIndex >= 3) menuIndex = 0;
      } else if (menuState == 3) {
        menuIndex++;
        if (menuIndex >= wifiCount) menuIndex = 0;
      }
      drawMenu();
    }

    if (enterTriggered) {
      if (menuState == 0) {
        if (menuIndex == 0) {
          // Jam Realtime
          runRealtimeClock();
          display.fillScreen(GC9A01A_BLACK);
          drawMenu();
        } else if (menuIndex == 1) {
          // System Info
          showSystemInfo(true);
          drawMenu();
        } else if (menuIndex == 2) {
          // Play GIF -> Masuk menu FPS dulu, lalu folder
          menuState = 2;
          menuIndex = 1; // default ke 30fps (index 1)
          drawMenu();
        } else if (menuIndex == 3) {
          // Scan WiFi (set STA mode dulu, lalu bersihkan cache lama)
          WiFi.mode(WIFI_STA);
          WiFi.scanDelete();
          delay(100);
          int n = WiFi.scanNetworks();
          wifiCount = 0;
          wifiList[wifiCount++] = "[ Back ]";
          for (int i = 0; i < n && wifiCount < 20; ++i) {
             wifiList[wifiCount++] = WiFi.SSID(i);
          }
          menuState = 3;
          menuIndex = 0; // reset kursor
          drawMenu();
        } else if (menuIndex == 4) {
          // Mode Web Uploader Access Point (Up Media)
          runMediaUploaderServer();
          
          // Kembali ke main menu setelah web server dihentikan
          menuState = 0;
          menuIndex = 0;
          drawMenu();
        } else if (menuIndex == 5) {
          // Spotify Remote - 240x240 Double Buffered to prevent flicker
          SpotifyRemote.begin();
          
          // Allocate 115KB canvas in PSRAM
          GFXcanvas16* spotifyCanvas = new GFXcanvas16(240, 240);
          
          display.fillScreen(GC9A01A_BLACK);
          while(true) {
              if (readButtonHeld()) break;
              int spotifyBtn = readButtonState();
              if (SpotifyRemote.update(spotifyBtn, spotifyCanvas)) {
                  // Push the entire 240x240 canvas to screen at once to eliminate flicker
                  display.drawRGBBitmap(0, 0, spotifyCanvas->getBuffer(), 240, 240);
              }
              yield();
          }
          delete spotifyCanvas; // Free memory
          
          SpotifyRemote.end(); // Stop BLE Server and Advertising

          display.fillScreen(GC9A01A_BLACK);
          menuState = 0;
          menuIndex = 0;
          drawMenu();
        } else if (menuIndex == 6) {
          // Reset System
          canvas.fillScreen(0);
          canvas.setTextSize(1);
          canvas.setTextColor(1);
          int16_t x1, y1; uint16_t w, h;
          canvas.getTextBounds("Rebooting...", 0, 0, &x1, &y1, &w, &h);
          canvas.setCursor((SCREEN_WIDTH - w)/2, 28);
          canvas.println("Rebooting...");
          canvasDisplay();
          delay(1000);
          
          // Clear RTC Time before rebooting so it doesn't retain corrupted time
          struct timeval tv;
          tv.tv_sec = 0;
          tv.tv_usec = 0;
          settimeofday(&tv, NULL);
          
          ESP.restart();
        }
      } else if (menuState == 2) {
        // FPS menu: pilih 15/30/60
        int fpsList[] = {15, 30, 60};
        gifFps = fpsList[menuIndex];
        Serial.printf("FPS dipilih: %d\n", gifFps);
        // Lanjut ke folder GIF
        listFolders();
        menuState = 1;
        menuIndex = 0;
        drawMenu();
      } else if (menuState == 1) {
        if (folderList[menuIndex] == "[ Back ]") {
          menuState = 0;
          menuIndex = 0;
          drawMenu();
        } else if (folderList[menuIndex] != "(No .bin Media)") {
            String selection = folderList[menuIndex];
            String selLow = selection; selLow.toLowerCase();

            if (selection.endsWith("/")) {
                playBinFrames(selection);
            } else {
                showBinFile(selection);
            }

            menuState = 1;
            drawMenu();
        } else {
            // It is "(No .bin Media)" - Return to main menu
            menuState = 0;
            menuIndex = 0;
            drawMenu();
        }
      } else if (menuState == 3) {
        if (wifiList[menuIndex] == "[ Back ]") {
          menuState = 0;
          menuIndex = 0;
          drawMenu();
        } else {
          String ssid = wifiList[menuIndex];
          // Fungsi untuk refresh tampilan OLED password entry
          String ssidDisplay = ssid.length() > 14 ? ssid.substring(0, 13) + "..." : ssid;
          auto refreshPassScreen = [&](const String& p) {
              canvas.fillScreen(0);
              canvas.setTextSize(1);
              canvas.setTextColor(1);
              canvas.setCursor(0, 10);
              canvas.print("SSID : "); canvas.println(ssidDisplay);
              canvas.setCursor(0, 24);
              canvas.print("PASS : ");
              for (int k = 0; k < (int)p.length(); k++) canvas.print("*");
              canvas.setCursor(0, 44);
              canvas.println("(Serial Monitor)");
              canvas.setCursor(0, 54);
              canvas.println("2x Klik = Batal");
              canvasDisplay();
          };
          String pass = "";
          refreshPassScreen(pass);

          Serial.println();
          Serial.print(">>> Password untuk '"); Serial.print(ssid); Serial.println("':");
          Serial.println("(Enter = Kirim, 2x Klik BOOT = Batal)");
          
          bool done = false;
          while(!done) {
            if(readButtonState() == 2) {
               pass = "";
               break; // batal
            }
            while(Serial.available()) {
               char c = Serial.read();
               if(c == '\n' || c == '\r') {
                 if(pass.length() > 0) { done = true; break; }
               } else if (c == 8 || c == 127) { // Backspace handling
                 if(pass.length() > 0) { pass.remove(pass.length()-1); }
                 Serial.print("\b \b");
               } else {
                 pass += c;
                 Serial.print('*'); // Echo bintang, bukan karakter aslinya
               }
               refreshPassScreen(pass);
            }
            yield();
          }
          Serial.println(); // Newline setelah bintang-bintang
          
          pass.trim();
          if(pass.length() > 0) {
              canvas.fillScreen(0);
              int16_t x1, y1; uint16_t w, h;
              canvas.getTextBounds("Connecting...", 0, 0, &x1, &y1, &w, &h);
              canvas.setCursor((SCREEN_WIDTH - w)/2, 28);
              canvas.println("Connecting...");
              canvasDisplay();
              
              bool success = connectToNewWiFi(ssid, pass);
              canvas.fillScreen(0);
              if(success) {
                  canvas.getTextBounds("Connected!", 0, 0, &x1, &y1, &w, &h);
                  canvas.setCursor((SCREEN_WIDTH - w)/2, 28);
                  canvas.println("Connected!");
              } else {
                  canvas.getTextBounds("Failed!", 0, 0, &x1, &y1, &w, &h);
                  canvas.setCursor((SCREEN_WIDTH - w)/2, 28);
                  canvas.println("Failed!");
              }
              canvasDisplay();
              delay(2000);
          }
          
          // Kembali ke main menu setelah selesai konek atau batal
          menuState = 0;
          menuIndex = 0;
          drawMenu();
        }
      }
    }
  }

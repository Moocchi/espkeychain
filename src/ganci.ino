#include "app/core/app_config.h"
#include "app/core/app_state.h"
#include "app/input/input.h"
#include "app/ui/ui_canvas.h"
#include "app/media/media_bin.h"
#include "app/media/media_gif.h"
#include "app/clock/clock_lvgl.h"
#include "app/net/web_uploader.h"
#include "app/ui/backlight.h"
#include "app/net/ntp_time.h"
#include "SpotifyRemote.h"

void setup() {
    Serial.begin(115200);
    pinMode(TOUCH_PIN, INPUT);

    // LED D2 tidak digunakan
    pinMode(2, OUTPUT);
    digitalWrite(2, LOW);
    // NeoPixel dimatikan saat boot
    neopixelWrite(48, 0, 0, 0);

    // Initialize SD Card (shared SPI bus with TFT display)
    // Both CS pins HIGH before SPI init to prevent bus conflict
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);

    // WiFi hanya konek saat diperlukan (Clock mode) - tidak konek saat boot
    WiFi.mode(WIFI_OFF);

    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
    display.begin(80000000); // 80MHz for Display
    setCpuFrequencyMhz(160); // Boost to 160MHz for better stability
    display.setRotation(0);
    display.fillScreen(GC9A01A_BLACK);

    // Initialize LVGL
    lv_init();
    lv_display_t * disp = lv_display_create(240, 240);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    // Allocate 115KB di PSRAM
    static uint8_t * lv_buf = (uint8_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, lv_buf, NULL, 240 * 240 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_disp_flush);

    // Re-init SPI bus AFTER display.begin() to restore MISO pin config
    // display.begin() may override SPI settings and drop MISO
    SPI.end();
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);

    // Mount SD Card - explicit SPI bus & safe frequency for initial handshake
    Serial.println("[SD] Attempting SD card init...");
    Serial.printf("[SD] Pins: CS=%d, SCK=%d, MISO=%d, MOSI=%d\n", SD_CS, TFT_SCLK, TFT_MISO, TFT_MOSI);

    bool sdMounted = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[SD] Mount attempt %d/3...\n", attempt);
        if (SD.begin(SD_CS, SPI, 40000000)) {
            sdMounted = true;
            break;
        }
        Serial.println("[SD] Mount failed, retrying...");
        SD.end();
        delay(1000); // Tunggu SD card stabilize
        SPI.end();
        SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
        delay(200);
    }

    if (!sdMounted) {
        Serial.println("[SD] Mount FAILED after 3 attempts! Check: wiring, card format (FAT32), card inserted?");
        canvas.fillScreen(0);
        canvas.setCursor(10, 20);
        canvas.println("SD CARD ERROR!");
        canvas.setCursor(10, 35);
        canvas.println("Check wiring/card");
        canvasDisplay();
        delay(3000);
    } else {
        uint8_t cardType = SD.cardType();
        const char* typeStr = (cardType == CARD_MMC) ? "MMC" :
                              (cardType == CARD_SD)  ? "SDSC" :
                              (cardType == CARD_SDHC)? "SDHC" : "UNKNOWN";
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("[SD] Card Type: %s | Size: %lluMB\n", typeStr, cardSize);
        Serial.printf("[SD] Total: %lluMB | Used: %lluMB\n", SD.totalBytes()/(1024*1024), SD.usedBytes()/(1024*1024));
    }

    // Initialize backlight PWM AFTER SPI + SD card are fully stable
    backlightInit();

    listFolders();
    drawMenu();

    Serial.println("\n=== Gunakan BOOT button: 1x Klik = NEXT, 2x Klik = ENTER | Tahan 500ms = Exit Media ===");
}

void loop() {
    // Auto-dim backlight after idle timeout
    backlightUpdate();

    // ============================================================
    // AUTO-SYNC NTP: Non-blocking 3-state machine
    //   State 0 = IDLE (tunggu interval)
    //   State 1 = CONNECTING (tunggu WiFi konek)
    //   State 2 = NTP_WAIT (tunggu NTP terset di RTC)
    // Tidak ada delay() atau while() — berjalan di setiap iterasi loop
    // ============================================================

    bool nextTriggered = false;
    bool enterTriggered = false;
    bool holdTriggered = readButtonHeld();

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
            int maxFpsMenu = selectedMediaIsGif ? 4 : 3;
            if (menuIndex >= maxFpsMenu) menuIndex = 0;
        } else if (menuState == 3) {
            menuIndex++;
            if (menuIndex >= wifiCount) menuIndex = 0;
        } else if (menuState == 4) {
            menuIndex++;
            if (menuIndex >= 2) menuIndex = 0; // 2 settings available
        } else if (menuState == 5) {
            if (menuIndex == 0) {
                uint8_t current = getLowBrightnessLevel();
                if (current == 10) current = 30;
                else if (current == 30) current = 50;
                else if (current == 50) current = 70;
                else current = 10;
                setLowBrightnessLevel(current);
            } else if (menuIndex == 1) {
                uint16_t current = getDimTimeout();
                if (current == 10) current = 30;
                else if (current == 30) current = 60;
                else if (current == 60) current = 120;
                else current = 10;
                setDimTimeout(current);
            }
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
                // Play Media -> Masuk menu Folder List dulu sebelum pilih FPS
                listFolders();
                menuState = 1;
                menuIndex = 0;
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
                while (true) {
                    backlightUpdate(); // Auto-dim backlight
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
                // Settings
                menuState = 4;
                menuIndex = 0;
                drawMenu();
            } else if (menuIndex == 7) {
                // Format SD Card
                canvas.fillScreen(0);
                canvas.setTextSize(1);
                canvas.setTextColor(1);
                canvas.setCursor(0, 5);
                canvas.println("FORMAT SD CARD?");
                canvas.setCursor(0, 20);
                canvas.println("ALL DATA WILL BE");
                canvas.setCursor(0, 30);
                canvas.println("DELETED!");
                canvas.setCursor(0, 45);
                canvas.println("2xKlik=YA 1x=BATAL");
                canvasDisplay();

                // Tunggu konfirmasi
                delay(300);
                bool doFormat = false;
                while (true) {
                    int confirmBtn = readButtonState();
                    if (confirmBtn == 2) { doFormat = true; break; }
                    if (confirmBtn == 1) { break; } // batal
                    yield();
                }

                if (doFormat) {
                    canvas.fillScreen(0);
                    canvas.setCursor(10, 20);
                    canvas.println("FORMATTING...");
                    canvas.setCursor(10, 35);
                    canvas.println("Please wait");
                    canvasDisplay();

                    // Melakukan "Real Format" menggunakan SdFat
                    bool ok = performRealFormat();

                    canvas.fillScreen(0);
                    if (ok) {
                        canvas.setCursor(10, 15);
                        canvas.println("FORMAT DONE!");
                        if (SD.cardType() != CARD_NONE) {
                            canvas.setCursor(10, 30);
                            canvas.println("SD Card OK!");
                        } else {
                            canvas.setCursor(10, 30);
                            canvas.println("Tekan RESET");
                            canvas.setCursor(10, 40);
                            canvas.println("untuk apply.");
                        }
                    } else {
                        canvas.setCursor(10, 28);
                        canvas.println("FORMAT FAILED!");
                    }
                    canvasDisplay();
                    delay(3000);
                }

                menuState = 0;
                menuIndex = 0;
                drawMenu();
            } else if (menuIndex == 8) {
                // Reset System
                canvas.fillScreen(0);
                canvas.setTextSize(1);
                canvas.setTextColor(1);
                int16_t x1, y1; uint16_t w, h;
                canvas.getTextBounds("Rebooting...", 0, 0, &x1, &y1, &w, &h);
                canvas.setCursor((SCREEN_WIDTH - w) / 2, 28);
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
            // FPS menu untuk GIF atau folder animasi frameXXXX.bin/qoi
            String selection = selectedMediaParam;
            if (selectedMediaIsGif) {
                int fpsListGif[] = {0, 15, 30, 60}; // 0 = ASLI GIF
                gifFpsOverride = fpsListGif[menuIndex];
                if (gifFpsOverride > 0) {
                    Serial.printf("GIF FPS override dipilih: %d\n", gifFpsOverride);
                } else {
                    Serial.println("GIF FPS override: ASLI GIF");
                }
                showGifFile(selection);
            } else {
                int fpsList[] = {15, 30, 60};
                gifFps = fpsList[menuIndex];
                Serial.printf("FPS dipilih: %d\n", gifFps);
                playBinFrames(selection);
            }

            // Kembali ke list folder setelah putar media selesai / di-_cancel_
            listFolders();
            menuState = 1;
            menuIndex = 0;
            drawMenu();
        } else if (menuState == 1) {
            if (folderList[menuIndex] == "[ Back ]") {
                menuState = 0;
                menuIndex = 0;
                drawMenu();
            } else if (folderList[menuIndex] != "(No Media)") {
                String selection = folderList[menuIndex];
                String selLow = selection;
                selLow.toLowerCase();

                if (selection.endsWith("/")) {
                    // Folder frameXXXX.bin/qoi -> minta FPS manual
                    selectedMediaParam = selection;
                    selectedMediaIsGif = false;
                    menuState = 2;
                    menuIndex = 1; // Default 30 FPS
                    drawMenu();
                } else if (selLow.endsWith(".gif")) {
                    // File GIF -> pilih FPS (ASLI/15/30/60)
                    selectedMediaParam = selection;
                    selectedMediaIsGif = true;
                    menuState = 2;
                    if (gifFpsOverride == 15) menuIndex = 1;
                    else if (gifFpsOverride == 30) menuIndex = 2;
                    else if (gifFpsOverride == 60) menuIndex = 3;
                    else menuIndex = 0; // ASLI GIF
                    drawMenu();
                } else {
                    // File .bin/.qoi statis
                    selectedMediaIsGif = false;
                    showBinFile(selection);
                    listFolders();
                    menuState = 1;
                    menuIndex = 0;
                    drawMenu();
                }
            } else {
                // It is "(No Media)" - Return to main menu
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
                while (!done) {
                    if (readButtonState() == 2) {
                        pass = "";
                        break; // batal
                    }
                    while (Serial.available()) {
                        char c = Serial.read();
                        if (c == '\n' || c == '\r') {
                            if (pass.length() > 0) { done = true; break; }
                        } else if (c == 8 || c == 127) { // Backspace handling
                            if (pass.length() > 0) { pass.remove(pass.length() - 1); }
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
                if (pass.length() > 0) {
                    canvas.fillScreen(0);
                    int16_t x1, y1; uint16_t w, h;
                    canvas.getTextBounds("Connecting...", 0, 0, &x1, &y1, &w, &h);
                    canvas.setCursor((SCREEN_WIDTH - w) / 2, 28);
                    canvas.println("Connecting...");
                    canvasDisplay();

                    bool success = connectToNewWiFi(ssid, pass);
                    canvas.fillScreen(0);
                    if (success) {
                        canvas.getTextBounds("Connected!", 0, 0, &x1, &y1, &w, &h);
                        canvas.setCursor((SCREEN_WIDTH - w) / 2, 28);
                        canvas.println("Connected!");
                    } else {
                        canvas.getTextBounds("Failed!", 0, 0, &x1, &y1, &w, &h);
                        canvas.setCursor((SCREEN_WIDTH - w) / 2, 28);
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
        } else if (menuState == 4) {
            menuState = 5;
            drawMenu();
        }
    }

    if (holdTriggered) {
        if (menuState == 4) {
            menuState = 0;
            menuIndex = 6; // Return to Settings item in main menu
            drawMenu();
        } else if (menuState == 5) {
            menuState = 4;
            drawMenu();
        }
    }
}
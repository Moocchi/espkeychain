#include "ui_canvas.h"
#include "../core/app_state.h"
#include "../input/input.h"

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

void setContrast(uint8_t level) {
    (void)level;
    // GC9A01A does not support simple contrast via software. Do nothing.
}

void drawMenu() {
    canvas.fillScreen(0); // clear virtual buffer
    canvas.setTextSize(1);
    canvas.setTextColor(1); // WHITE for monochrome canvas

    if (menuState == 0) {
        String title = "=== MAIN MENU ===";
        int16_t x1, y1; uint16_t w, h;
        canvas.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
        canvas.setCursor((SCREEN_WIDTH - w) / 2, 0);
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
        if (folderCount == 0 || (folderCount == 2 && folderList[1] == "(No Media)")) {
            canvas.setCursor(0, 20);
            canvas.println("NO MEDIA FOUND!");
            canvas.println("Upload .bin/.qoi/.gif");
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
        canvas.setCursor((SCREEN_WIDTH - w) / 2, 0);
        canvas.println(title2);
        if (selectedMediaIsGif) {
            const char* fpsOptsGif[] = {"ASLI GIF", "15 FPS", "30 FPS", "60 FPS"};
            for (int i = 0; i < 4; i++) {
                canvas.setCursor(0, (i + 1) * 12 + 8);
                if (i == menuIndex) canvas.print("> ");
                else canvas.print("  ");
                canvas.print(fpsOptsGif[i]);

                if (i == 0 && gifFpsOverride == 0) canvas.print(" *");
                if (i == 1 && gifFpsOverride == 15) canvas.print(" *");
                if (i == 2 && gifFpsOverride == 30) canvas.print(" *");
                if (i == 3 && gifFpsOverride == 60) canvas.print(" *");
            }
        } else {
            const char* fpsOpts[] = {"15 FPS", "30 FPS", "60 FPS"};
            for (int i = 0; i < 3; i++) {
                canvas.setCursor(0, (i + 1) * 12 + 8);
                if (i == menuIndex) canvas.print("> ");
                else canvas.print("  ");
                canvas.print(fpsOpts[i]);
                int thisFps = (i == 0) ? 15 : (i == 1) ? 30 : 60;
                if (thisFps == gifFps) canvas.print(" *");
            }
        }
    } else if (menuState == 3) {
        String title = "=== PILIH WIFI ===";
        int16_t x1, y1; uint16_t w, h;
        canvas.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
        canvas.setCursor((SCREEN_WIDTH - w) / 2, 0);
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

    uint64_t sdTotal = SD.totalBytes();
    uint64_t sdUsed  = SD.usedBytes();
    float totalFS = sdTotal / (1024.0 * 1024.0);
    float usedFS = sdUsed / (1024.0 * 1024.0);
    int pctFS = (sdTotal > 0) ? (int)((usedFS / totalFS) * 100) : 0;

    canvas.fillScreen(0);
    canvas.setTextSize(1);
    canvas.setTextColor(1);

    String title = "=== SYSTEM INFO ===";
    int16_t x1, y1; uint16_t w, h;
    canvas.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
    canvas.setCursor((SCREEN_WIDTH - w) / 2, 0);
    canvas.println(title);

    canvas.setCursor(0, 15);
    if (sdTotal == 0 || SD.cardType() == CARD_NONE) {
        canvas.println("SD : Not Detected");
    } else {
        canvas.printf("SD :%.1fM|%.1fM(%d%%)\n", totalFS, usedFS, pctFS);
    }
    canvas.printf("RAM:%.2fM|%.2fM(%d%%)\n", totalRAM, usedRAM, pctRAM);

    if (totalPSRAM > 0) {
        canvas.printf("PSR:%.1fM|%.1fM(%d%%)\n", totalPSRAM, usedPSRAM, pctPSRAM);
    } else {
        canvas.println("PSR: Not Detected");
    }

    float espTemp = temperatureRead();
    canvas.printf("TMP:%.1f C\n", espTemp);

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

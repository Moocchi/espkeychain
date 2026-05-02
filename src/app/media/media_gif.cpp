#include "media_gif.h"
#include "../core/app_state.h"
#include "../ui/ui_canvas.h"
#include "../input/input.h"

// --- GIF PLAYBACK (SD -> AnimatedGIF -> TFT) ---
static AnimatedGIF gifPlayer;
static File gifFile;
static uint16_t gifLineBuffer[240];
static int gifDrawOffsetX = 0;
static int gifDrawOffsetY = 0;
static uint8_t* gifDataPSRAM = NULL;
static size_t gifDataPSRAMSize = 0;
static uint16_t* gifFrameCanvas = NULL; // 240x240 RGB565 canvas untuk update per-frame agar minim tearing
static bool gifUseCanvasCompose = false;
static uint8_t* gifCookedFrameBuffer = NULL; // [indexed canvas][cooked RGB565 canvas]
static size_t gifCookedFrameBufferSize = 0;
static bool gifUseCookedFrameOutput = false;
static bool gifComposeDirty = false;
static int gifDirtyMinX = 0;
static int gifDirtyMinY = 0;
static int gifDirtyMaxX = -1;
static int gifDirtyMaxY = -1;

static void resetGifComposeDirtyRect() {
    gifComposeDirty = false;
    gifDirtyMinX = 240;
    gifDirtyMinY = 240;
    gifDirtyMaxX = -1;
    gifDirtyMaxY = -1;
}

static void markGifComposeDirtyRect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 240) x1 = 240;
    if (y1 > 240) y1 = 240;

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    if (!gifComposeDirty) {
        gifDirtyMinX = x0;
        gifDirtyMinY = y0;
        gifDirtyMaxX = x1 - 1;
        gifDirtyMaxY = y1 - 1;
        gifComposeDirty = true;
        return;
    }

    if (x0 < gifDirtyMinX) gifDirtyMinX = x0;
    if (y0 < gifDirtyMinY) gifDirtyMinY = y0;
    if (x1 - 1 > gifDirtyMaxX) gifDirtyMaxX = x1 - 1;
    if (y1 - 1 > gifDirtyMaxY) gifDirtyMaxY = y1 - 1;
}

static void flushGifComposeDirtyRect() {
    if (!gifUseCanvasCompose || !gifFrameCanvas || !gifComposeDirty) {
        return;
    }

    int x = gifDirtyMinX;
    int y = gifDirtyMinY;
    int w = gifDirtyMaxX - gifDirtyMinX + 1;
    int h = gifDirtyMaxY - gifDirtyMinY + 1;
    if (w <= 0 || h <= 0) {
        return;
    }

    display.startWrite();
    display.setAddrWindow(x, y, w, h);
    const uint16_t *src = gifFrameCanvas + (y * 240) + x;
    for (int row = 0; row < h; row++) {
        display.writePixels((uint16_t*)src, w, true, false);
        src += 240;
    }
    display.endWrite();
    resetGifComposeDirtyRect();
}

static bool ensureGifCookedFrameBuffer(size_t requiredSize) {
    if (requiredSize == 0) {
        return false;
    }

    if (gifCookedFrameBuffer && gifCookedFrameBufferSize >= requiredSize) {
        memset(gifCookedFrameBuffer, 0, requiredSize);
        return true;
    }

    if (gifCookedFrameBuffer) {
        free(gifCookedFrameBuffer);
        gifCookedFrameBuffer = NULL;
        gifCookedFrameBufferSize = 0;
    }

    gifCookedFrameBuffer = (uint8_t*)ps_malloc(requiredSize);
    if (!gifCookedFrameBuffer) {
        return false;
    }

    gifCookedFrameBufferSize = requiredSize;
    memset(gifCookedFrameBuffer, 0, requiredSize);
    return true;
}

static void flushGifCookedFrameRect(int frameX, int frameY, int frameW, int frameH, int canvasW, int canvasH) {
    if (!gifUseCookedFrameOutput || !gifCookedFrameBuffer) {
        return;
    }
    if (frameW <= 0 || frameH <= 0 || canvasW <= 0 || canvasH <= 0) {
        return;
    }

    int srcX0 = frameX;
    int srcY0 = frameY;
    int srcX1 = frameX + frameW;
    int srcY1 = frameY + frameH;

    if (srcX0 < 0) srcX0 = 0;
    if (srcY0 < 0) srcY0 = 0;
    if (srcX1 > canvasW) srcX1 = canvasW;
    if (srcY1 > canvasH) srcY1 = canvasH;
    if (srcX1 <= srcX0 || srcY1 <= srcY0) {
        return;
    }

    int dstX = gifDrawOffsetX + srcX0;
    int dstY = gifDrawOffsetY + srcY0;
    int copyW = srcX1 - srcX0;
    int copyH = srcY1 - srcY0;

    if (dstX < 0) {
        int skip = -dstX;
        dstX = 0;
        srcX0 += skip;
        copyW -= skip;
    }
    if (dstY < 0) {
        int skip = -dstY;
        dstY = 0;
        srcY0 += skip;
        copyH -= skip;
    }
    if (dstX + copyW > 240) {
        copyW = 240 - dstX;
    }
    if (dstY + copyH > 240) {
        copyH = 240 - dstY;
    }
    if (copyW <= 0 || copyH <= 0) {
        return;
    }

    uint16_t *cookedCanvas = (uint16_t *)(gifCookedFrameBuffer + ((size_t)canvasW * (size_t)canvasH));
    uint16_t *src = cookedCanvas + ((size_t)srcY0 * (size_t)canvasW) + (size_t)srcX0;

    display.startWrite();
    display.setAddrWindow(dstX, dstY, copyW, copyH);
    for (int row = 0; row < copyH; row++) {
        display.writePixels(src, copyW, true, false);
        src += canvasW;
    }
    display.endWrite();
}

static void prepareGifComposeCanvas() {
    if (!gifFrameCanvas) {
        gifFrameCanvas = (uint16_t*)ps_malloc(115200); // 240x240x2
    }
    gifUseCanvasCompose = (gifFrameCanvas != NULL);
    if (gifUseCanvasCompose) {
        memset(gifFrameCanvas, 0, 115200);
        resetGifComposeDirtyRect();
        Serial.println("GIF: Canvas compose ON (reduced tearing).");
    } else {
        Serial.println("GIF: Canvas compose OFF (OOM), fallback per-line.");
    }
}

static void *gifPsramAlloc(uint32_t size) {
    return ps_malloc(size);
}

static void gifPsramFree(void *p) {
    if (p) free(p);
}

static void freeGifPSRAMData() {
    if (gifDataPSRAM) {
        free(gifDataPSRAM);
        gifDataPSRAM = NULL;
    }
    gifDataPSRAMSize = 0;
}

static bool preloadGifToPSRAM(const String& path) {
    freeGifPSRAMData();

    File f = SD.open(path, FILE_READ);
    if (!f) {
        return false;
    }

    size_t fSize = f.size();
    if (fSize == 0) {
        f.close();
        return false;
    }

    // Sisakan headroom PSRAM agar decoder + stack tetap aman.
    const size_t reservePSRAM = 256 * 1024;
    size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (fSize + reservePSRAM > freePSRAM) {
        f.close();
        return false;
    }

    uint8_t *buf = (uint8_t*)ps_malloc(fSize);
    if (!buf) {
        f.close();
        return false;
    }

    size_t totalRead = 0;
    while (totalRead < fSize) {
        size_t chunk = (fSize - totalRead > 8192) ? 8192 : (fSize - totalRead);
        int n = f.read(buf + totalRead, chunk);
        if (n <= 0) {
            break;
        }
        totalRead += (size_t)n;
    }
    f.close();

    if (totalRead != fSize) {
        free(buf);
        return false;
    }

    gifDataPSRAM = buf;
    gifDataPSRAMSize = fSize;
    return true;
}

static bool peekGifCanvasSize(const String& path, uint16_t &w, uint16_t &h) {
    w = 0;
    h = 0;

    File f = SD.open(path, FILE_READ);
    if (!f) {
        return false;
    }

    uint8_t hdr[10];
    int n = f.read(hdr, sizeof(hdr));
    f.close();
    if (n < 10) {
        return false;
    }

    bool sigOk =
        (hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == '8' && (hdr[4] == '7' || hdr[4] == '9') && hdr[5] == 'a');
    if (!sigOk) {
        return false;
    }

    w = (uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8);
    h = (uint16_t)hdr[8] | ((uint16_t)hdr[9] << 8);
    return true;
}

static const char* gifErrorToString(int err) {
    switch (err) {
        case GIF_SUCCESS: return "GIF_SUCCESS";
        case GIF_DECODE_ERROR: return "GIF_DECODE_ERROR";
        case GIF_TOO_WIDE: return "GIF_TOO_WIDE";
        case GIF_INVALID_PARAMETER: return "GIF_INVALID_PARAMETER";
        case GIF_UNSUPPORTED_FEATURE: return "GIF_UNSUPPORTED_FEATURE";
        case GIF_FILE_NOT_OPEN: return "GIF_FILE_NOT_OPEN";
        case GIF_EARLY_EOF: return "GIF_EARLY_EOF";
        case GIF_EMPTY_FRAME: return "GIF_EMPTY_FRAME";
        case GIF_BAD_FILE: return "GIF_BAD_FILE";
        case GIF_ERROR_MEMORY: return "GIF_ERROR_MEMORY";
        default: return "GIF_UNKNOWN_ERROR";
    }
}

static void *gifOpenFile(const char *filename, int32_t *pFileSize) {
    if (gifFile) {
        gifFile.close();
    }
    gifFile = SD.open(filename, FILE_READ);
    if (!gifFile) {
        return NULL;
    }
    *pFileSize = (int32_t)gifFile.size();
    return (void *)&gifFile;
}

static void gifCloseFile(void *pHandle) {
    File *f = (File *)pHandle;
    if (f && *f) {
        f->close();
    }
}

static int32_t gifReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    File *f = (File *)pFile->fHandle;
    if (!f || !(*f)) {
        return 0;
    }

    int32_t bytesLeft = pFile->iSize - pFile->iPos;
    if (bytesLeft <= 0) {
        return 0;
    }
    if (iLen > bytesLeft) {
        iLen = bytesLeft;
    }

    int32_t bytesRead = (int32_t)f->read(pBuf, iLen);
    if (bytesRead < 0) {
        bytesRead = 0;
    }
    pFile->iPos += bytesRead;
    return bytesRead;
}

static int32_t gifSeekFile(GIFFILE *pFile, int32_t iPosition) {
    File *f = (File *)pFile->fHandle;
    if (!f || !(*f)) {
        return 0;
    }
    if (!f->seek(iPosition)) {
        return pFile->iPos;
    }
    pFile->iPos = iPosition;
    return iPosition;
}

static bool openGifSource(const String& path, GIF_DRAW_CALLBACK *drawCb) {
    bool opened = false;
    if (gifDataPSRAM && gifDataPSRAMSize > 0) {
        opened = gifPlayer.open(gifDataPSRAM, (int)gifDataPSRAMSize, drawCb);
    }
    if (!opened) {
        opened = gifPlayer.open(path.c_str(), gifOpenFile, gifCloseFile, gifReadFile, gifSeekFile, drawCb);
    }
    return opened;
}

static void gifDrawCallback(GIFDRAW *pDraw) {
    int dstY = gifDrawOffsetY + pDraw->iY + pDraw->y;
    if (dstY < 0 || dstY >= 240) {
        return;
    }

    int dstX = gifDrawOffsetX + pDraw->iX;
    int drawWidth = pDraw->iWidth;
    int srcOffset = 0;

    if (dstX < 0) {
        srcOffset = -dstX;
        drawWidth -= srcOffset;
        dstX = 0;
    }
    if (dstX + drawWidth > 240) {
        drawWidth = 240 - dstX;
    }
    if (drawWidth <= 0) {
        return;
    }

    uint8_t *src = pDraw->pPixels + srcOffset;
    uint16_t *palette = pDraw->pPalette;

    // Compose ke framebuffer penuh dulu, lalu flush 1x per frame di loop utama.
    // Ini mengurangi tearing dibanding push per-line saat frame masih didecode.
    if (gifUseCanvasCompose && gifFrameCanvas) {
        uint16_t *dst = gifFrameCanvas + (dstY * 240) + dstX;

        if (pDraw->ucDisposalMethod == 2) {
            for (int x = 0; x < drawWidth; x++) {
                uint8_t idx = src[x];
                if (idx == pDraw->ucTransparent) {
                    idx = pDraw->ucBackground;
                }
                dst[x] = palette[idx];
            }
        } else if (pDraw->ucHasTransparency) {
            for (int x = 0; x < drawWidth; x++) {
                uint8_t idx = src[x];
                if (idx != pDraw->ucTransparent) {
                    dst[x] = palette[idx];
                }
            }
        } else {
            for (int x = 0; x < drawWidth; x++) {
                dst[x] = palette[src[x]];
            }
        }

        markGifComposeDirtyRect(dstX, dstY, drawWidth, 1);
        return;
    }

    display.startWrite();

    if (pDraw->ucDisposalMethod == 2) {
        for (int x = 0; x < drawWidth; x++) {
            uint8_t idx = src[x];
            if (idx == pDraw->ucTransparent) {
                idx = pDraw->ucBackground;
            }
            gifLineBuffer[x] = palette[idx];
        }
        display.setAddrWindow(dstX, dstY, drawWidth, 1);
        display.writePixels(gifLineBuffer, drawWidth, true, false);
        display.endWrite();
        return;
    }

    if (pDraw->ucHasTransparency) {
        int x = 0;
        while (x < drawWidth) {
            while (x < drawWidth && src[x] == pDraw->ucTransparent) {
                x++;
            }
            int runStart = x;
            while (x < drawWidth && src[x] != pDraw->ucTransparent) {
                gifLineBuffer[x - runStart] = palette[src[x]];
                x++;
            }
            int runLen = x - runStart;
            if (runLen > 0) {
                display.setAddrWindow(dstX + runStart, dstY, runLen, 1);
                display.writePixels(gifLineBuffer, runLen, true, false);
            }
        }
    } else {
        for (int x = 0; x < drawWidth; x++) {
            gifLineBuffer[x] = palette[src[x]];
        }
        display.setAddrWindow(dstX, dstY, drawWidth, 1);
        display.writePixels(gifLineBuffer, drawWidth, true, false);
    }

    display.endWrite();
}

void showGifFile(String filename) {
    String path = filename.startsWith("/") ? filename : "/" + filename;
    String lowerPath = path;
    lowerPath.toLowerCase();
    if (!lowerPath.endsWith(".gif")) {
        Serial.println("GIF: Invalid file type: " + path);
        return;
    }

    uint16_t srcW = 0;
    uint16_t srcH = 0;
    if (peekGifCanvasSize(path, srcW, srcH)) {
        Serial.printf("GIF: source canvas=%ux%u\n", srcW, srcH);
        if (srcW > MAX_WIDTH) {
            Serial.printf("GIF: Unsupported width %u (AnimatedGIF max %d).\n", srcW, MAX_WIDTH);
            canvas.fillScreen(0);
            canvas.setCursor(10, 20);
            canvas.println("GIF TOO WIDE");
            canvas.setCursor(10, 35);
            canvas.print(srcW);
            canvas.print(" > ");
            canvas.println(MAX_WIDTH);
            canvasDisplay();
            delay(1200);
            return;
        }
    }

    canvas.fillScreen(0);
    canvas.setCursor(10, 20);
    canvas.println("Playing GIF:");
    canvas.setCursor(10, 35);
    String shortName = path.length() > 17 ? "..." + path.substring(path.length() - 14) : path;
    canvas.println(shortName);
    canvasDisplay();
    delay(500);

    display.fillScreen(GC9A01A_BLACK);

    uint32_t prevCpuMhz = getCpuFrequencyMhz();
    if (prevCpuMhz < 240) {
        setCpuFrequencyMhz(240);
    }

    gifPlayer.begin(GIF_PALETTE_RGB565_LE);

    bool turboOk = gifPlayer.allocTurboBuf(gifPsramAlloc);
    if (turboOk) {
        Serial.println("GIF: Turbo decode enabled.");
    }

    bool loadedPSRAM = preloadGifToPSRAM(path);
    if (loadedPSRAM) {
        Serial.printf("GIF: Preloaded to PSRAM (%u bytes).\n", (unsigned int)gifDataPSRAMSize);
    } else {
        Serial.println("GIF: PSRAM preload skipped, streaming from SD.");
    }

    gifUseCookedFrameOutput = false;
    gifUseCanvasCompose = false;
    resetGifComposeDirtyRect();
    int openErr = GIF_SUCCESS;

    bool opened = openGifSource(path, NULL);
    if (!opened) {
        openErr = gifPlayer.getLastError();
    }
    if (opened) {
        int canvasW = gifPlayer.getCanvasWidth();
        int canvasH = gifPlayer.getCanvasHeight();
        size_t cookedBufSize = (size_t)canvasW * (size_t)canvasH * 3;

        if (ensureGifCookedFrameBuffer(cookedBufSize)) {
            gifPlayer.setFrameBuf(gifCookedFrameBuffer);
            gifPlayer.setDrawType(GIF_DRAW_COOKED);
            gifUseCookedFrameOutput = true;
            Serial.printf("GIF: COOKED full-frame ON (%u bytes).\n", (unsigned int)cookedBufSize);
        } else {
            Serial.printf("GIF: COOKED buffer OOM (%u bytes), fallback RAW callback.\n", (unsigned int)cookedBufSize);
            gifPlayer.close();
            opened = false;
            openErr = GIF_ERROR_MEMORY;
        }
    }

    if (!opened) {
        prepareGifComposeCanvas();
        opened = openGifSource(path, gifDrawCallback);
        if (opened) {
            gifPlayer.setDrawType(GIF_DRAW_RAW);
            Serial.println("GIF: RAW callback mode aktif.");
        } else {
            openErr = gifPlayer.getLastError();
        }
    }

    if (!opened) {
        Serial.printf("GIF: Failed to open/decode (err=%d %s): %s\n", openErr, gifErrorToString(openErr), path.c_str());
        canvas.fillScreen(0);
        canvas.setCursor(10, 25);
        canvas.println("GIF OPEN FAILED");
        canvas.setCursor(10, 40);
        canvas.println(gifErrorToString(openErr));
        canvasDisplay();
        delay(1200);

        if (turboOk) gifPlayer.freeTurboBuf(gifPsramFree);
        freeGifPSRAMData();
        gifUseCanvasCompose = false;
        if (getCpuFrequencyMhz() != prevCpuMhz) setCpuFrequencyMhz(prevCpuMhz);
        return;
    }

    int canvasW = gifPlayer.getCanvasWidth();
    int canvasH = gifPlayer.getCanvasHeight();
    if (canvasW == 240 && canvasH == 240) {
        gifDrawOffsetX = 0;
        gifDrawOffsetY = 0;
    } else {
        gifDrawOffsetX = (240 - canvasW) / 2;
        gifDrawOffsetY = (240 - canvasH) / 2;
    }
    Serial.printf("GIF: canvas=%dx%d offset=(%d,%d)\n", canvasW, canvasH, gifDrawOffsetX, gifDrawOffsetY);
    if (gifFpsOverride > 0) {
        Serial.printf("GIF: FPS override aktif %d fps.\n", gifFpsOverride);
    } else {
        Serial.println("GIF: Timing mengikuti delay frame asli file.");
    }
    if (gifUseCookedFrameOutput) {
        Serial.println("GIF: Render mode COOKED + partial flush.");
    } else if (gifUseCanvasCompose) {
        Serial.println("GIF: Render mode RAW compose + partial flush.");
    } else {
        Serial.println("GIF: Render mode RAW per-line direct.");
    }
    Serial.println("GIF: Tahan BOOT 500ms = kembali.");

    resetButtonHold();
    bool exitGif = false;
    int forcedFrameDelayMs = (gifFpsOverride > 0) ? (1000 / gifFpsOverride) : 0;
    uint32_t nextFrameDeadline = millis();

    uint32_t perfStartMs = millis();
    uint32_t renderedFrames = 0;

    while (!exitGif) {
        // Tetap polling tombol tiap iterasi frame agar hold bisa terdeteksi
        // walau tidak ada waktu tunggu (mis. decode lambat, waitMs = 0).
        if (readButtonHeld()) {
            exitGif = true;
            break;
        }

        if (gifUseCanvasCompose && !gifUseCookedFrameOutput) {
            resetGifComposeDirtyRect();
        }

        int frameDelayMs = 0;
        uint32_t frameStartMs = millis();

        if (!gifPlayer.playFrame(false, &frameDelayMs, NULL)) {
            gifPlayer.reset();
            if (gifUseCookedFrameOutput && gifCookedFrameBuffer && gifCookedFrameBufferSize > 0) {
                memset(gifCookedFrameBuffer, 0, gifCookedFrameBufferSize);
            } else if (gifUseCanvasCompose && gifFrameCanvas) {
                memset(gifFrameCanvas, 0, 115200);
                resetGifComposeDirtyRect();
                display.fillScreen(GC9A01A_BLACK);
            }
            continue;
        }
        renderedFrames++;

        if (gifUseCookedFrameOutput && gifCookedFrameBuffer) {
            flushGifCookedFrameRect(
                gifPlayer.getFrameXOff(),
                gifPlayer.getFrameYOff(),
                gifPlayer.getFrameWidth(),
                gifPlayer.getFrameHeight(),
                gifPlayer.getCanvasWidth(),
                gifPlayer.getCanvasHeight());
        } else if (gifUseCanvasCompose && gifFrameCanvas) {
            flushGifComposeDirtyRect();
        }

        if (readButtonHeld()) {
            exitGif = true;
            break;
        }

        if (gifFpsOverride > 0) {
            frameDelayMs = 1000 / gifFpsOverride;
        }

        // Delay 0 biasanya dipakai encoder sebagai "cepat"; beri fallback 10ms agar tidak busy-loop.
        if (frameDelayMs <= 0) {
            frameDelayMs = 10;
        }

        int32_t waitMs;
        uint32_t nowMs = millis();
        if (forcedFrameDelayMs > 0) {
            nextFrameDeadline += (uint32_t)forcedFrameDelayMs;
            waitMs = (int32_t)(nextFrameDeadline - nowMs);
            if (waitMs < 0) {
                // Sudah telat dari target; reset deadline agar pacing tetap stabil.
                nextFrameDeadline = nowMs;
                waitMs = 0;
            }
        } else {
            waitMs = frameDelayMs - (int32_t)(nowMs - frameStartMs);
            if (waitMs < 0) {
                waitMs = 0;
            }
        }

        uint32_t waitUntil = millis() + (uint32_t)waitMs;
        while (!exitGif && millis() < waitUntil) {
            if (readButtonHeld()) {
                exitGif = true;
                break;
            }
            delay(1);
        }

        if (millis() - perfStartMs >= 1000) {
            float fps = (renderedFrames * 1000.0f) / (float)(millis() - perfStartMs);
            if (gifFpsOverride > 0) {
                Serial.printf("GIF: real fps %.1f (target %d)\n", fps, gifFpsOverride);
            } else {
                Serial.printf("GIF: real fps %.1f (native)\n", fps);
            }
            perfStartMs = millis();
            renderedFrames = 0;
        }
    }

    gifPlayer.close();
    if (turboOk) gifPlayer.freeTurboBuf(gifPsramFree);
    freeGifPSRAMData();
    gifUseCanvasCompose = false;
    gifUseCookedFrameOutput = false;
    if (getCpuFrequencyMhz() != prevCpuMhz) setCpuFrequencyMhz(prevCpuMhz);
    display.fillScreen(GC9A01A_BLACK);
}

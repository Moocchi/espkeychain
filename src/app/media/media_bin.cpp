#include "media_bin.h"
#include "../core/app_state.h"
#include "../ui/ui_canvas.h"
#include "../ui/backlight.h"
#include "../input/input.h"

#define QOI_MALLOC(sz) ps_malloc(sz)
#define QOI_FREE(p)    free(p)
#define QOI_IMPLEMENTATION
#include "../../qoi.h"

// --- PSRAM BINARY CACHING ---
static uint16_t* psramFrames[MAX_PSRAM_FRAMES];
static int totalLoadedFrames = 0;
static String currentlyLoadedFolder = "";

static void clearPSRAMCache() {
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

void listFolders() { // SCAN MEDIA: file .bin/.qoi/.gif + folder frameXXXX(.bin/.qoi)
    folderCount = 0;
    folderList[folderCount++] = "[ Back ]";
    File root = SD.open("/");
    File file = root.openNextFile();
    while (file && folderCount < 20) {
        if (file.isDirectory()) {
            String dirName = String(file.name());
            bool hasMedia = false;
            File subDir = SD.open("/" + dirName);
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
            if (fl.endsWith(".bin") || fl.endsWith(".qoi") || fl.endsWith(".gif")) {
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

static bool decodeQOI(File& f, uint16_t* outBuf) {
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

static String getBinFormatName(size_t fSize) {
    if (fSize >= 230400) return "RGBA 32-bit";
    if (fSize >= 172800) return "RGB 24-bit";
    if (fSize >= 115200) return "RGB565 16-bit";
    if (fSize >= 7200)   return "1-bit Mono";
    return "Unknown Format";
}

void showBinFile(String filename) {
    String path = filename.startsWith("/") ? filename : "/" + filename;

    File f = SD.open(path, "r");
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

    uint16_t* imgBuf = (uint16_t*)ps_malloc(115200); // 240x240x2
    if (!imgBuf) { Serial.println("MEDIA: OOM!"); f.close(); return; }

    if (isQoi) {
        decodeQOI(f, imgBuf);
    } else if (fSize >= 230400) { // 32-bit RGBA
        if (fSize > 230400) f.seek(fSize - 230400); // Skip header
        uint8_t r_buf[240 * 4];
        for (int y = 0; y < 240; y++) {
            f.read(r_buf, 240 * 4);
            for (int x = 0; x < 240; x++) {
                uint8_t r = r_buf[x * 4 + 0], g = r_buf[x * 4 + 1], b = r_buf[x * 4 + 2];
                uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                imgBuf[y * 240 + x] = color;
            }
        }
    } else if (fSize >= 172800) { // 24-bit RGB
        if (fSize > 172800) f.seek(fSize - 172800); // Skip header
        uint8_t r_buf[240 * 3];
        for (int y = 0; y < 240; y++) {
            f.read(r_buf, 240 * 3);
            for (int x = 0; x < 240; x++) {
                uint8_t r = r_buf[x * 3 + 0], g = r_buf[x * 3 + 1], b = r_buf[x * 3 + 2];
                uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                imgBuf[y * 240 + x] = color;
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

        for (int p = 0; p < 57600; p++) {
            uint16_t c = imgBuf[p];
            imgBuf[p] = (c >> 8) | (c << 8);
        }
    }
    f.close();

    display.drawRGBBitmap(0, 0, imgBuf, 240, 240);
    free(imgBuf);

    Serial.println("MEDIA: done. Tahan BOOT 500ms = kembali.");
    resetButtonHold();
    while (true) { delay(10); backlightUpdate(); if (readButtonHeld()) break; }
    display.fillScreen(GC9A01A_BLACK);
}

void playBinFrames(String selection) {
    String folder = selection;
    if (folder.endsWith("/")) folder.remove(folder.length() - 1);
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
        File dir = SD.open(fullPath);
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

                File f = SD.open(framePath, "r");
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

                psramFrames[i] = (uint16_t*)ps_malloc(115200);
                if (psramFrames[i] == NULL) { Serial.println("PSRAM: OOM!"); f.close(); break; }

                if (isQoiSeq) {
                    decodeQOI(f, psramFrames[i]);
                } else if (fSize >= 230400) { // 32-bit RGBA
                    if (fSize > 230400) f.seek(fSize - 230400);
                    uint8_t r_buf[240 * 4];
                    for (int y = 0; y < 240; y++) {
                        f.read(r_buf, 240 * 4);
                        for (int x = 0; x < 240; x++) {
                            uint8_t r = r_buf[x * 4 + 0], g = r_buf[x * 4 + 1], b = r_buf[x * 4 + 2];
                            uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                            psramFrames[i][y * 240 + x] = color;
                        }
                    }
                } else if (fSize >= 172800) { // 24-bit RGB
                    if (fSize > 172800) f.seek(fSize - 172800);
                    uint8_t r_buf[240 * 3];
                    for (int y = 0; y < 240; y++) {
                        f.read(r_buf, 240 * 3);
                        for (int x = 0; x < 240; x++) {
                            uint8_t r = r_buf[x * 3 + 0], g = r_buf[x * 3 + 1], b = r_buf[x * 3 + 2];
                            uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                            psramFrames[i][y * 240 + x] = color;
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

                    // Gunakan internal DMA buffer 8KB agar pembacaan dari SD Card via VFS jauh lebih cepat
                    // daripada f.read() langsung ke pointer PSRAM (yang terpecah dan mematikan DMA direct).
                    uint8_t* dmaBuf = (uint8_t*)malloc(8192);
                    if (dmaBuf) {
                        for (int offset = 0; offset < 115200; offset += 8192) {
                            int chunk = (115200 - offset > 8192) ? 8192 : (115200 - offset);
                            f.read(dmaBuf, chunk);
                            memcpy((uint8_t*)psramFrames[i] + offset, dmaBuf, chunk);
                        }
                        free(dmaBuf);
                    } else {
                        f.read((uint8_t*)psramFrames[i], 115200);
                    }

                    // Super-fast 32-bit swap bytes
                    uint32_t* p32 = (uint32_t*)psramFrames[i];
                    for (int p = 0; p < 28800; p++) {
                        uint32_t c = p32[p];
                        p32[p] = ((c & 0x00FF00FF) << 8) | ((c & 0xFF00FF00) >> 8);
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
    resetButtonHold();

    bool exitBin = false;
    int currentFrame = 0;
    uint32_t nextFrameTime = millis();

    while (!exitBin) {
        backlightUpdate(); // Auto-dim backlight

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

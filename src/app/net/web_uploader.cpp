#include "web_uploader.h"
#include "../core/app_state.h"
#include "../ui/ui_canvas.h"
#include "../ui/backlight.h"
#include "../input/input.h"

// ===================================
// API & Handlers for Web Upload
// ===================================

static void sendJsonResponse(int code, String json) {
    server.send(code, "application/json", json);
}

static void handleStatus() {
    float totalFS = SD.totalBytes() / (1024.0 * 1024.0);
    float usedFS = SD.usedBytes() / (1024.0 * 1024.0);
    float freeFS = totalFS - usedFS;
    float totalRAM = ESP.getHeapSize() / 1024.0;
    float freeRAM = ESP.getFreeHeap() / 1024.0;
    float espTempC = temperatureRead();

    String json = "{";
    json += "\"status\":\"ok\",";
    json += "\"fs_total_mb\":" + String(totalFS, 2) + ",";
    json += "\"fs_used_mb\":" + String(usedFS, 2) + ",";
    json += "\"fs_free_mb\":" + String(freeFS, 2) + ",";
    json += "\"ram_total_kb\":" + String(totalRAM, 2) + ",";
    json += "\"ram_free_kb\":" + String(freeRAM, 2) + ",";
    json += "\"temp_c\":" + String(espTempC, 2);
    json += "}";
    sendJsonResponse(200, json);
}

static void handleList() {
    String json = "{\"files\":[";
    File root = SD.open("/");
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

static void recursiveDelete(String path) {
    File dir = SD.open(path);
    if (!dir) return;
    if (!dir.isDirectory()) {
        dir.close();
        SD.remove(path);
        return;
    }

    File file = dir.openNextFile();
    while (file) {
        String childPath = String(file.name());
        if (!childPath.startsWith(path)) {
            if (path.endsWith("/")) childPath = path + childPath;
            else childPath = path + "/" + childPath;
        }
        bool isDir = file.isDirectory();
        file.close();

        if (isDir) {
            recursiveDelete(childPath);
        } else {
            SD.remove(childPath);
        }

        dir = SD.open(path);
        if (!dir) break;
        file = dir.openNextFile();
    }
    dir.close();
    SD.rmdir(path);
}

// Returns true if format succeeded
bool performRealFormat() {
    Serial.println("[SD] Starting Real Format (FAT32)...");
    SD.end(); // Tutup koneksi SD.h
    delay(500);

    // Re-initialize SPI bus for SD Card since SD.end() might mess it up
    SPI.end();
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
    delay(100);

    // Gunakan SdFs (support FAT + exFAT) agar SDXC 64GB+ bisa diinisialisasi
    SdFs sd_fat;
    if (!sd_fat.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(16), &SPI))) {
        Serial.println("[SD] SdFat: Hardware init FAILED!");
        return false;
    }

    // Gunakan FatFormatter untuk MEMAKSA format FAT32 pada kartu ukuran apapun
    // (termasuk SDXC 64GB/128GB yang biasanya exFAT)
    FatFormatter fatFormatter;
    uint8_t secBuf[512];
    Serial.println("[SD] Formatting to FAT32 (forced)...");
    if (!fatFormatter.format(sd_fat.card(), secBuf, &Serial)) {
        Serial.println("[SD] SD Format FAILED!");
        return false;
    }

    Serial.println("[SD] SD Format SUCCESS!");
    sd_fat.end(); // Tutup SdFat
    delay(500);

    // Reset SD card state: toggle CS & re-init SPI bus
    digitalWrite(SD_CS, HIGH);
    SPI.end();
    delay(500);
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
    delay(500);

    // Coba remount dengan SD.h langsung (retry 5x)
    bool mounted = false;
    for (int i = 1; i <= 5; i++) {
        Serial.printf("[SD] Remount attempt %d/5...\n", i);
        if (SD.begin(SD_CS, SPI, 40000000)) {
            mounted = true;
            break;
        }
        SD.end();
        delay(1000);
        SPI.end();
        SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
        delay(300);
    }

    if (mounted) {
        uint8_t cardType = SD.cardType();
        const char* typeStr = (cardType == CARD_MMC) ? "MMC" :
                              (cardType == CARD_SD)  ? "SDSC" :
                              (cardType == CARD_SDHC) ? "SDHC" : "UNKNOWN";
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("[SD] Remounted OK! Type: %s | Size: %lluMB\n", typeStr, cardSize);
    } else {
        Serial.println("[SD] Remount failed. Tekan tombol RESET fisik untuk apply format.");
    }
    return true;
}

static void handleDelete() {
    if (!server.hasArg("path")) {
        sendJsonResponse(400, "{\"error\":\"missing path\"}");
        return;
    }
    String path = server.arg("path");
    if (!path.startsWith("/")) path = "/" + path;

    if (SD.exists(path)) {
        File f = SD.open(path, "r");
        bool isDir = false;
        if (f) {
            isDir = f.isDirectory();
            f.close();
        }

        if (isDir) {
            recursiveDelete(path);
            sendJsonResponse(200, "{\"status\":\"deleted\"}");
        } else {
            if (SD.remove(path)) {
                sendJsonResponse(200, "{\"status\":\"deleted\"}");
            } else {
                sendJsonResponse(500, "{\"error\":\"delete failed\"}");
            }
        }
    } else {
        sendJsonResponse(404, "{\"error\":\"not found\"}");
    }
}

static void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    static File fsUploadFile;

    // Fast Upload DMA Write Buffer
    static uint8_t* writeBuffer = nullptr;
    static size_t writeBufferLen = 0;
    const size_t WRITE_BUFFER_MAX = 16384; // 16KB buffer

    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;

        // Create directories if they don't exist
        int lastSlash = filename.lastIndexOf('/');
        if (lastSlash > 0) {
            String dirPath = filename.substring(0, lastSlash);
            if (!SD.exists(dirPath)) {
                SD.mkdir(dirPath);
            }
        }

        fsUploadFile = SD.open(filename, FILE_WRITE);

        if (!writeBuffer) writeBuffer = (uint8_t*)malloc(WRITE_BUFFER_MAX);
        writeBufferLen = 0;

        Serial.print("Upload Start: "); Serial.println(filename);

        canvas.fillScreen(0);
        canvas.setTextSize(1);
        canvas.setCursor(10, 20);
        canvas.println("WRITING...");
        canvas.setCursor(10, 30);
        String shortName = filename.length() > 20 ? "..." + filename.substring(filename.length() - 17) : filename;
        canvas.println(shortName);
        canvasDisplay();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile && writeBuffer) {
            size_t offset = 0;
            while (offset < upload.currentSize) {
                size_t copyLen = upload.currentSize - offset;
                if (writeBufferLen + copyLen > WRITE_BUFFER_MAX) {
                    copyLen = WRITE_BUFFER_MAX - writeBufferLen;
                }
                memcpy(writeBuffer + writeBufferLen, upload.buf + offset, copyLen);
                writeBufferLen += copyLen;
                offset += copyLen;

                if (writeBufferLen == WRITE_BUFFER_MAX) {
                    fsUploadFile.write(writeBuffer, WRITE_BUFFER_MAX);
                    writeBufferLen = 0;
                }
            }
        } else if (fsUploadFile) {
            fsUploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            if (writeBuffer && writeBufferLen > 0) {
                fsUploadFile.write(writeBuffer, writeBufferLen);
                writeBufferLen = 0;
            }
            fsUploadFile.close();
            if (writeBuffer) {
                free(writeBuffer);
                writeBuffer = nullptr;
            }
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

    // OPTIMASI: Kencangkan transfer rate WiFi mentok!
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

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

    server.onNotFound([]() {
        sendJsonResponse(404, "{\"error\":\"not found\"}");
    });
    server.begin();

    // Show IP on screen
    String ip = WiFi.softAPIP().toString();

    while (true) {
        backlightUpdate(); // Auto-dim backlight
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

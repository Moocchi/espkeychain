#include "ntp_time.h"

const char* ntpServer1 = "162.159.200.1";  // Cloudflare time server
const char* ntpServer2 = "216.239.35.0";   // Google time server
const char* ntpServer3 = "132.163.96.5";   // NIST time server
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;

bool isTimeSynced = false;

void syncNTP() {
    Serial.println("Configuring NTP (3 servers)...");
    // Gunakan 3 server sekaligus — SNTP client otomatis pilih yang tercepat
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);

    // Beri waktu 500ms agar DNS resolve dan SNTP bisa mengirim request
    delay(500);

    // Tunggu hingga waktu berhasil didapat (max 10 detik)
    struct tm timeinfo;
    int attempt = 0;
    while (!getLocalTime(&timeinfo) && attempt < 20) {
        delay(500);
        Serial.print(".");
        attempt++;
    }

    if (attempt < 20) {
        isTimeSynced = true;
        Serial.printf("\nNTP Sync OK: %02d:%02d:%02d\n",
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println("\nNTP Sync FAILED.");
        isTimeSynced = false;
    }
}

bool connectToNewWiFi(String ssid, String pass) {
    Serial.print("Menghubungkan ke ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        delay(500);
        Serial.print(".");
        retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi terhubung!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        syncNTP();
        WiFi.mode(WIFI_OFF);
        return true;
    } else {
        Serial.println("\nGagal. Password salah atau sinyal lemah.");
        WiFi.mode(WIFI_OFF);
        return false;
    }
}

// Mengambil string tanggal. Langsung baca dari RTC internal ESP32.
// Tidak perlu WiFi setelah NTP pertama kali sync.
String getDateStr() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "No RTC";
    char dateStr[20];
    strftime(dateStr, sizeof(dateStr), "%a %d, %m, %Y", &timeinfo);
    return String(dateStr);
}

// Mengambil string jam HH:MM:SS langsung dari RTC internal ESP32.
String getTimeStr() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "--:--:--";
    char timeStr[10];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    return String(timeStr);
}

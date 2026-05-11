#include "clock_lvgl.h"
#include "../core/app_state.h"
#include "../input/input.h"
#include "../ui/lvgl_clock.h"
#include "../ui/backlight.h"
#include "../net/ntp_time.h"

// LVGL Flush function
void my_disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    display.drawRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_display_flush_ready(disp);
}

void runRealtimeClock() {
    display.fillScreen(GC9A01A_BLACK);
    show_lvgl_clock();

    // --- BACKGROUND WIFI & NTP (only in clock mode) ---
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

    uint32_t lastNtpAttempt = 0;
    uint32_t last_lv_tick = millis();

    while (true) {
        uint32_t now = millis();
        backlightUpdate(); // Auto-dim backlight
        if (readButtonHeld()) break; // Tahan tombol exit

        // --- BACKGROUND LOGIC ---
        struct tm t;
        bool hasTime = getLocalTime(&t, 0);
        if (!hasTime && ntpEverSynced) {
            hasTime = getLocalTime(&t, 2);
        }

        wl_status_t wfStatus = WiFi.status();

        static uint32_t lastRetry = 0;
        if (clockWifiActive && !hasTime && wfStatus != WL_CONNECTED && (now - lastWifiBegin > 20000) && (now - lastRetry > 20000)) {
            lastRetry = now;
            WiFi.disconnect();
            delay(200);
            esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
            WiFi.begin("M.Jarez", "samsito140671");
            lastWifiBegin = now;
        }

        if (clockWifiActive && !hasTime && wfStatus == WL_CONNECTED && (now - lastNtpAttempt > 5000)) {
            lastNtpAttempt = now;
            configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
        }

        if (hasTime && true) {
            ntpEverSynced = true;
            lastNtpSyncMs = millis();
            clockWifiActive = false;
            if (WiFi.getMode() != WIFI_OFF) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
            }
        }

        // --- UPDATE LVGL OBJECTS ---
        char dStr[32];
        if (hasTime) {
            const char * days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            const char * months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            snprintf(dStr, sizeof(dStr), "%s, %02d %s %04d", days[t.tm_wday], t.tm_mday, months[t.tm_mon], t.tm_year + 1900);
            set_clock_time(t.tm_hour, t.tm_min, t.tm_sec, dStr);
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
            // Dummy time while connecting
            set_clock_time(10, 8, 38, dStr);
        }

        lv_tick_inc(millis() - last_lv_tick);
        last_lv_tick = millis();
        lv_timer_handler();

        yield();
    }

    // Sembunyikan elemen jam sebelum keluar dari menu (agar UI tdk bocor ke GFX)
    hide_lvgl_clock();

    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    clockWifiActive = false;
    neopixelWrite(48, 0, 0, 0);
}

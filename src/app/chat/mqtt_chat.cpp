#include "mqtt_chat.h"
#include "../core/app_state.h"
#include "../ui/ui_canvas.h"
#include "../input/input.h"
#include "../ui/backlight.h"
#include <PubSubClient.h>
#include <Preferences.h>

const char* mqtt_server = "broker.emqx.io";
const char* mqtt_topic_rx = "esp32s3/ganci/chat/rx";
const char* mqtt_topic_tx = "esp32s3/ganci/chat/tx";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

static String lastMsgIn = "-";
static String lastMsgOut = "-";
static bool exitChat = false;

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }
    lastMsgIn = msg;
    backlightWake();
}

static void reconnectMqtt() {
    if (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        String clientId = "ESP32S3Client-";
        clientId += String(random(0xffff), HEX);
        if (mqttClient.connect(clientId.c_str())) {
            mqttClient.subscribe(mqtt_topic_rx);
        }
    }
}

void runMqttChat() {
    canvas.fillScreen(0);
    canvas.setTextSize(1);
    canvas.setTextColor(1);
    canvas.setCursor(10, 20);
    canvas.println("Turning on WiFi...");
    canvasDisplay();

    // Reset WiFi state cleanly
    WiFi.disconnect(false, true); 
    delay(100);
    WiFi.mode(WIFI_STA);
    
    Preferences prefs;
    prefs.begin("sys", true);
    String ssid = prefs.getString("wifi_ssid", "");
    String pass = prefs.getString("wifi_pass", "");
    prefs.end();

    if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
    } else {
        WiFi.begin(); // Fallback
    }
    
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        canvas.fillScreen(0);
        canvas.setCursor(10, 20);
        canvas.print("WiFi Connecting");
        for(int i=0; i<retry%4; i++) canvas.print(".");
        canvasDisplay();
        
        if (readButtonHeld()) {
            WiFi.disconnect(false, true);
            WiFi.mode(WIFI_OFF);
            return;
        }
        delay(500);
        retry++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        canvas.fillScreen(0);
        canvas.setCursor(10, 20);
        canvas.println("WIFI FAILED!");
        canvasDisplay();
        delay(2000);
        WiFi.disconnect(false, true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    mqttClient.setServer(mqtt_server, 1883);
    mqttClient.setCallback(mqttCallback);

    exitChat = false;
    uint32_t lastReconnect = 0;
    
    const int QR_COUNT = 4;
    const char* qrs[] = {"Halo!", "OK", "Bentar", "Sibuk"};
    int qr_index = 0;
    bool menu_open = false;

    resetButtonHold();

    while (!exitChat) {
        backlightUpdate();
        
        if (!mqttClient.connected()) {
            if (millis() - lastReconnect > 5000) {
                lastReconnect = millis();
                // Draw connecting indicator
                canvas.fillScreen(0);
                canvas.setCursor(0, 20);
                canvas.print("MQTT Connecting...");
                canvasDisplay();
                
                reconnectMqtt();
            }
        } else {
            mqttClient.loop();
        }

        // Draw basic UI
        canvas.fillScreen(0);
        canvas.setCursor(0, 0);
        if (mqttClient.connected()) {
            canvas.print("MQTT: ONLINE");
        } else {
            canvas.print("MQTT: ERR ");
            canvas.print(mqttClient.state());
        }
        
        canvas.setCursor(0, 15);
        canvas.print("IN : "); canvas.print(lastMsgIn);
        canvas.setCursor(0, 25);
        canvas.print("OUT: "); canvas.print(lastMsgOut);
        
        // Menu
        if (menu_open) {
            canvas.setCursor(0, 45);
            canvas.print("> "); canvas.print(qrs[qr_index]);
        } else {
            canvas.setCursor(0, 45);
            canvas.print("[Tap to Reply]");
        }

        canvasDisplay();

        // Input
        if (readButtonHeld()) {
            if (menu_open) {
                menu_open = false;
            } else {
                exitChat = true;
            }
            resetButtonHold();
        } else {
            int btn = readButtonState();
            if (btn == 1) { // Single Tap
                if (!menu_open) menu_open = true;
                else qr_index = (qr_index + 1) % QR_COUNT;
            } else if (btn == 2) { // Double tap
                if (menu_open && mqttClient.connected()) {
                    lastMsgOut = qrs[qr_index];
                    mqttClient.publish(mqtt_topic_tx, qrs[qr_index]);
                    menu_open = false;
                }
            }
        }
        delay(30);
    }

    mqttClient.disconnect();
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_OFF);
}

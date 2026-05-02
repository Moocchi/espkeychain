#include "app_state.h"

Adafruit_GC9A01A display(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas1 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);

WebServer server(80);
DNSServer dnsServer;
bool isWebUploadMode = false;

String folderList[20];
int folderCount = 0;

int menuState = 0;
int menuIndex = 0;
String mainMenuList[] = {
    "Jam Realtime",
    "Sytm Info",
    "Tampilkan Media",
    "Wifi Scan",
    "Up Media",
    "Spotify Remote",
    "Format SD",
    "Reset System"
};
int mainMenuCount = 8;

String wifiList[20];
int wifiCount = 0;

int fpsMenuIndex = 0;
const uint8_t CONTRAST_FULL = 255;

int gifFps = 30;
int gifFpsOverride = 0;
bool selectedMediaIsGif = false;
String selectedMediaParam = "";

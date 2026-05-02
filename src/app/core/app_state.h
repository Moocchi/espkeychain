#ifndef APP_STATE_H
#define APP_STATE_H

#include "app_config.h"

extern Adafruit_GC9A01A display;
extern GFXcanvas1 canvas;

extern WebServer server;
extern DNSServer dnsServer;
extern bool isWebUploadMode;

extern String folderList[20];
extern int folderCount;

extern int menuState;
extern int menuIndex;
extern String mainMenuList[];
extern int mainMenuCount;

extern String wifiList[20];
extern int wifiCount;

extern int fpsMenuIndex;
extern const uint8_t CONTRAST_FULL;

extern int gifFps;
extern int gifFpsOverride;
extern bool selectedMediaIsGif;
extern String selectedMediaParam;

#endif

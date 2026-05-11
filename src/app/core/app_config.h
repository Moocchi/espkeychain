#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <Update.h>
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <FS.h>
#include <SdFat.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <lvgl.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include <AnimatedGIF.h>
#include <Preferences.h>

// Define ps_malloc as heap_caps_malloc for SPIRAM
#define ps_malloc(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define TFT_CS   D0
#define TFT_DC   D1
#define TFT_RST  D2
#define TFT_MOSI D10
#define TFT_SCLK D8
#define TFT_MISO D9
#define TFT_BLK  D4
#define SD_CS    D7

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Touch Sensor pin (XIAO ESP32S3 - D3 pin)
#define TOUCH_PIN D3
#define TOUCH_ACTIVE_STATE HIGH

#define MAX_PSRAM_FRAMES 100

#endif

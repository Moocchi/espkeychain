#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

// NTP Server - pakai IP langsung untuk skip DNS resolution (lebih cepat!)
extern const char* ntpServer1;
extern const char* ntpServer2;
extern const char* ntpServer3;
extern const long  gmtOffset_sec;
extern const int   daylightOffset_sec;

extern bool isTimeSynced;

void syncNTP();
bool connectToNewWiFi(String ssid, String pass);
String getDateStr();
String getTimeStr();

#endif

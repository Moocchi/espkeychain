#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include "../core/app_config.h"

// Brightness levels (0-255)
#define BL_BRIGHTNESS_FULL  255
// BL_BRIGHTNESS_LOW is now variable

extern uint8_t currentLowBrightness;

// Timeout configuration (in ms)
#define BL_INITIAL_TIMEOUT_MS   5000    // 5 detik idle setelah boot -> dim
// BL_WAKE_TIMEOUT_MS is now dynamic based on user setting

// Initialize backlight PWM on TFT_BLK pin
void backlightInit();

// Set brightness manually (0-255)
void setBrightness(uint8_t level);

// Call this every loop iteration — auto-dims after timeout
void backlightUpdate();

// Call this whenever touch/button input is detected to wake backlight
void backlightWake();

uint8_t getLowBrightnessLevel();
void setLowBrightnessLevel(uint8_t level);

uint16_t getDimTimeout();
void setDimTimeout(uint16_t seconds);

#endif

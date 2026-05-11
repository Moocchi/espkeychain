#include "backlight.h"
#include <Preferences.h>
#include <math.h>

static Preferences prefs;
uint8_t currentLowBrightness = 10;
uint16_t currentDimTimeout = 60; // in seconds

// Use LEDC channel 7 to avoid conflicts with SPI/SD card peripherals
// Channels 0-3 are commonly used internally by other ESP32 libraries
#define BL_PWM_CHANNEL  7
#define BL_PWM_FREQ     5000   // 5kHz — silent, no audible whine
#define BL_PWM_RES      8      // 8-bit resolution (0-255)

static uint32_t lastActivityTime = 0;
static bool isDimmed = false;
static uint32_t currentTimeoutMs = BL_INITIAL_TIMEOUT_MS;

static float currentFadeLevel = BL_BRIGHTNESS_FULL;
static uint8_t targetBrightness = BL_BRIGHTNESS_FULL;
static uint32_t lastFadeTime = 0;

void backlightInit() {
    // Setup LEDC PWM channel for backlight control
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RES);
    ledcAttachPin(TFT_BLK, BL_PWM_CHANNEL);

    prefs.begin("sys", false);
    currentLowBrightness = prefs.getUChar("low_bright", 10);
    currentDimTimeout = prefs.getUShort("dim_timeout", 60);

    lastActivityTime = millis();
    lastFadeTime = millis();
    isDimmed = false;
    currentTimeoutMs = BL_INITIAL_TIMEOUT_MS;
    
    currentFadeLevel = BL_BRIGHTNESS_FULL;
    targetBrightness = BL_BRIGHTNESS_FULL;
    ledcWrite(BL_PWM_CHANNEL, (uint8_t)currentFadeLevel);
}

void setBrightness(uint8_t level) {
    targetBrightness = level;
    currentFadeLevel = level;
    ledcWrite(BL_PWM_CHANNEL, level);
}

void backlightWake() {
    lastActivityTime = millis();
    if (isDimmed) {
        isDimmed = false;
        currentTimeoutMs = currentDimTimeout * 1000; // wake timeout based on user setting
        targetBrightness = BL_BRIGHTNESS_FULL; // Smooth wake up
    } else if (targetBrightness != BL_BRIGHTNESS_FULL) {
        // Interrupted while fading down
        targetBrightness = BL_BRIGHTNESS_FULL;
    }
}

void backlightUpdate() {
    uint32_t now = millis();

    // Check timeout to trigger dim
    if (!isDimmed) {
        if (now - lastActivityTime >= currentTimeoutMs) {
            isDimmed = true;
            targetBrightness = currentLowBrightness;
        }
    }

    // Handle smooth fading
    if (currentFadeLevel != targetBrightness) {
        float dt = (now - lastFadeTime) / 1000.0f;
        if (dt > 0.05f) dt = 0.05f; // limit max step to 50ms to prevent jumping

        float diff = targetBrightness - currentFadeLevel;
        
        if (fabs(diff) < 0.5f) {
            currentFadeLevel = targetBrightness;
        } else {
            // Fade up lebih cepat (8.0) agar layar responsif saat disentuh,
            // Fade down lebih pelan (4.0) agar terasa relaxing.
            float speed = (diff > 0) ? 8.0f : 4.0f;
            currentFadeLevel += diff * speed * dt;
        }
        
        ledcWrite(BL_PWM_CHANNEL, (uint8_t)currentFadeLevel);
    }
    
    lastFadeTime = now;
}

uint8_t getLowBrightnessLevel() {
    return currentLowBrightness;
}

void setLowBrightnessLevel(uint8_t level) {
    currentLowBrightness = level;
    prefs.putUChar("low_bright", currentLowBrightness);
    if (isDimmed) {
        // Update target but let it fade to the new low brightness smoothly
        targetBrightness = currentLowBrightness;
    }
}

uint16_t getDimTimeout() {
    return currentDimTimeout;
}

void setDimTimeout(uint16_t seconds) {
    currentDimTimeout = seconds;
    prefs.putUShort("dim_timeout", currentDimTimeout);
    // Apply new timeout to currentTimeoutMs if currently awake
    if (!isDimmed && currentTimeoutMs != BL_INITIAL_TIMEOUT_MS) {
        currentTimeoutMs = currentDimTimeout * 1000;
    }
}

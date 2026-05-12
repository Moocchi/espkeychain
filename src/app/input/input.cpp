#include "input.h"
#include "../ui/backlight.h"

static uint32_t lastClickTime = 0;
static bool buttonState = !TOUCH_ACTIVE_STATE;
static bool lastButtonState = !TOUCH_ACTIVE_STATE;
static const uint32_t DOUBLE_CLICK_GAP = 400;
static const uint32_t DEBOUNCE_DELAY = 50;
static uint32_t lastDebounceTime = 0;

static uint32_t btnHoldStart = 0;
static bool btnHoldFired = false;

void resetButtonHold() {
    btnHoldStart = 0;
    btnHoldFired = false;
}

// Return true once when hold threshold is met
bool readButtonHeld() {
    bool pressed = (digitalRead(TOUCH_PIN) == TOUCH_ACTIVE_STATE);
    if (pressed) {
        backlightWake(); // Wake backlight on any touch
        if (btnHoldStart == 0) {
            btnHoldStart = millis();
            btnHoldFired = false;
        } else if (!btnHoldFired && (millis() - btnHoldStart >= 500)) {
            btnHoldFired = true;
            btnHoldStart = 0;
            return true;
        }
    } else {
        btnHoldStart = 0;
        btnHoldFired = false;
    }
    return false;
}

// 0 = none, 1 = single click (NEXT), 2 = double click (ENTER)
int readButtonState() {
    int action = 0;
    bool reading = digitalRead(TOUCH_PIN);
    uint32_t now = millis();

    if (reading != lastButtonState) {
        lastDebounceTime = now;
    }

    if ((now - lastDebounceTime) > DEBOUNCE_DELAY) {
        if (reading != buttonState) {
            buttonState = reading;

            if (buttonState == TOUCH_ACTIVE_STATE) {
                backlightWake(); // Wake backlight on confirmed touch
                if (now - lastClickTime < DOUBLE_CLICK_GAP) {
                    action = 2;
                    lastClickTime = 0;
                } else {
                    lastClickTime = now;
                }
            }
        }
    }

    if (lastClickTime > 0 && (now - lastClickTime) > DOUBLE_CLICK_GAP) {
        if (buttonState != TOUCH_ACTIVE_STATE) {
            action = 1;
            lastClickTime = 0;
        }
    }

    lastButtonState = reading;
    return action;
}

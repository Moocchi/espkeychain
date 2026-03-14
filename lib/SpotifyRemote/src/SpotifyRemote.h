#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>

class SpotifyRemoteClass {
public:
    // Initialize BLE Server
    void begin();
    
    // Stop BLE Server and free memory
    void end();
    
    // Call this repeatedly in a loop to handle BLE, Animations, and Drawing.
    // Returns true if the screen was redrawn and needs flush (e.g. canvasDisplay).
    // btnAction: 0=none, 1=single tap, 2=double tap
    bool update(int btnAction, Adafruit_GFX* display);
};

extern SpotifyRemoteClass SpotifyRemote;

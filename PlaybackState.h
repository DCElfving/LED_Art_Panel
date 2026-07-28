#ifndef PLAYBACK_STATE_H
#define PLAYBACK_STATE_H

// ===========================================================================
// PlaybackState.h — reads one image file and holds the current 32x32 frame.
// File format: [uint16 frameCount][frame0 ...][frame1 ...] where each frame is
// 1024 RGB565 pixels (2048 bytes).
// ===========================================================================

#include <Arduino.h>
#include <LittleFS.h>

struct PlaybackState {
    File file;
    String filename; // Keep track of which file needed
    uint16_t frameData[1024]; // Buffer for current frame (32x32)
    uint16_t frameCount;
    int currentFrameIndex;
    unsigned long lastFrameAdvance;

    void close() {
        if (file) file.close();
        frameCount = 0;
        filename = "";
    }

    bool open(String fname) {
        if (file) file.close();
        filename = fname;
        file = LittleFS.open(filename, "r");
        if (!file) {
            return false;
        }
        // Read header (2 bytes frame count)
        if (file.read((uint8_t*)&frameCount, 2) != 2) {
            frameCount = 0; 
            return false;
        }
        currentFrameIndex = -1; // Force load
        return true;
    }

    void loadFrame(int index) {
        if (!file || index >= frameCount) return;
        if (currentFrameIndex == index) return;

        // Seek: Header (2) + Index * FrameSize (2048)
        size_t pos = 2 + (index * 2048);
        file.seek(pos);
        file.read((uint8_t*)frameData, 2048);
        currentFrameIndex = index;
    }
};

#endif

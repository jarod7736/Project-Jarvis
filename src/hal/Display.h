#pragma once

#include <Arduino.h>

namespace jarvis::hal {

enum class DeviceState {
    IDLE,
    LISTENING,  // blue
    THINKING,   // yellow
    SPEAKING,   // green
    ERROR       // red
};

class Display {
public:
    // Call once after M5.begin().
    static void begin();

    // Fill status bar with state colour, draw dot + label.
    static void setStatus(DeviceState state);

    // Replace transcript region in-place (no sprite, no scroll).
    static void showTranscript(const String& text);

    // Replace response region in-place with word-wrapped text.
    static void showResponse(const String& text);

    // Redraw footer: tier string ("LAN"/"TS"/"HOT"/"OFF"), RSSI, wall-clock.
    // Call every 30s from loop(); also call on connect/disconnect events.
    static void updateFooter(const String& tier, int rssi);

    // Top-right corner of the status bar: battery icon + percentage. Cached
    // so setStatus() can repaint it (fillRect wipes the bar). Call from
    // loop() on a timer; values are clamped 0..100. `level` < 0 means
    // "unknown" — the icon is drawn empty and the text shows "--%".
    static void updateBattery(int level, bool charging);

    // Drive the SPEAKING-state waveform animation. Cheap when not
    // speaking (single comparison); ~10 Hz redraw when active. Call
    // every loop() iteration. Internally tracks the last setStatus()
    // value so transitions out of SPEAKING erase the strip exactly
    // once. Animation strip lives at the bottom of the response region
    // — the response text above it stays visible.
    static void tickWaveform();
};

}  // namespace jarvis::hal

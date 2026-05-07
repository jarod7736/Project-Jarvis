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

    // Drive the per-frame particle-field render. Throttled internally
    // to ~30 FPS; cheap when not due (single millis() compare). Call
    // every loop() iteration. The particle simulation reads the most
    // recent state set by setStatus() and the tier set by setTier().
    // (Historical name: was tickWaveform() in the boxed-region UI.)
    static void tickParticles();
    // Back-compat alias — main.cpp called this name in earlier builds.
    static inline void tickWaveform() { tickParticles(); }

    // Phase 7 OTA: paint a small "OTA" badge in the footer (left of
    // the tier/RSSI string) while ArduinoOTA or HTTPUpdate is active.
    // Cached so updateFooter() can repaint it after fillRect.
    static void setOtaActive(bool active);

    // Captive-portal Config mode screen. Shows the AP SSID
    // (Jarvis-Setup), the URL to open (http://192.168.4.1), and a hint
    // that holding the screen for 2 seconds returns to Normal mode.
    // Call once on entry to Config mode; the rendering is static so
    // there's no per-frame redraw needed.
    static void drawConfigScreen();

    // Update the backlight brightness. Range is clamped to [10, 255]
    // to match the schema's validation (and to keep the screen from
    // becoming completely unreadable if a stale or hostile NVS write
    // sets it to zero). Safe to call from any task — under the hood
    // it's a PWM duty cycle write, not a framebuffer flush, so it
    // doesn't race with rendering done on the loop task.
    static void setBrightness(int v);

    // Set the particle-field tier color. The four tiers correspond to
    // the routing layer's intelligence backends:
    //   QWEN  — mint  (#5fe3a1) — local Qwen on the LLM Module
    //   LOCAL — blue  (#7fc8ff) — OpenClaw local model (gemma)
    //   CLOUD — purple(#c89cff) — Claude via OpenClaw
    //   HA    — orange(#ffb454) — Home Assistant intent
    // Cached so re-renders pick it up. Idempotent.
    enum class Tier : uint8_t { QWEN = 0, LOCAL = 1, CLOUD = 2, HA = 3 };
    static void setTier(Tier t);
};

}  // namespace jarvis::hal

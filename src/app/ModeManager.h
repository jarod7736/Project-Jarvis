#pragma once

// ModeManager — coordinates the device's two top-level operating modes:
//
//   NORMAL — voice pipeline runs (LLMModule + FSM ticks). WiFi is in STA
//            mode, joined to a saved network. No web server.
//   CONFIG — captive portal active (AP "Jarvis-Setup" + http server at
//            192.168.4.1). Voice pipeline is paused — main.cpp gates the
//            LLMModule update + state-machine tick on !isConfig().
//
// Mode is toggled by:
//   - 2-second long-press on the touchscreen (detected here in tick()).
//   - The web UI POST /api/exit (CaptivePortal sets the exit flag, we
//     observe it on tick()).
//
// CONFIG also auto-exits after kConfigIdleTimeoutMs of inactivity to
// avoid leaving an open AP up indefinitely if the user walks away.

#include <Arduino.h>

namespace jarvis::app {

class ModeManager {
public:
    enum class Mode : uint8_t { Normal, Config };

    // Boots the device into Normal mode. Caller is responsible for
    // bringing WiFi up (WiFiManager::begin) before this runs.
    static void begin();

    // Pump the long-press detector and any pending mode transitions.
    // Cheap when the press detector is idle. Must be called every loop()
    // iteration so M5.Touch state stays fresh.
    static void tick();

    static Mode mode();
    static bool isConfig();

    // Programmatic transitions. Idempotent (no-op if already in target).
    // enterConfig() pauses the voice pipeline and brings the AP up;
    // enterNormal() tears the AP down and reconnects to a saved network.
    static void enterConfig();
    static void enterNormal();

    // Reset the inactivity timer (called on every API hit / user
    // interaction) so a busy session doesn't auto-exit.
    static void noteActivity();
};

}  // namespace jarvis::app

#pragma once

// CaptivePortal — bring the Jarvis-Setup AP up, host the web UI from
// LittleFS, and serve the schema-driven config + WiFi APIs at
// http://192.168.4.1/.
//
// Lifecycle:
//   begin()  — switch WiFi to AP mode (open SSID "Jarvis-Setup"), bind
//              port 80, start DNS catch-all on port 53. Idempotent.
//   end()    — tear down server + DNS, switch WiFi back to STA. Idempotent.
//   tick()   — must be called every loop() iteration while running().
//              Drains the DNS catch-all and (eventually) idle-checks.
//   running() — true between begin() and end().
//
// In Config mode the voice pipeline is paused (LLMModule update + FSM
// tick are gated in main.cpp on !ModeManager::isConfig()).
//
// Captive-portal trick: every 404 redirects to http://192.168.4.1/ — iOS,
// Android, and Windows all hit OS-specific probe URLs to detect captive
// networks and pop up the system "sign in" UI when those probes redirect.

#include <Arduino.h>

namespace jarvis::net {

class CaptivePortal {
public:
    static void begin();
    static void end();
    static void tick();
    static bool running();

    // Set when the web UI POSTs /api/exit. ModeManager polls this on its
    // tick and transitions back to Normal mode (which calls end()).
    static bool exitRequested();
    static void clearExitFlag();
};

}  // namespace jarvis::net

#include "ModeManager.h"

#include <M5Unified.h>
#include <WiFi.h>

#include "../hal/Display.h"
#include "../net/CaptivePortal.h"
#include "../net/WiFiManager.h"

namespace jarvis::app {

namespace {
constexpr uint32_t kHoldMs              = 2000;        // long-press threshold
constexpr uint32_t kConfigIdleTimeoutMs = 5UL * 60000;  // 5 minutes

ModeManager::Mode g_mode = ModeManager::Mode::Normal;
uint32_t          g_last_activity_ms = 0;

// Long-press detector state. Fires once per held press (>= kHoldMs);
// requires release before re-arming so a continuous touch doesn't spam.
bool     g_was_touched = false;
bool     g_fired       = false;
uint32_t g_press_start = 0;

bool detectLongPress() {
    bool touched = M5.Touch.getCount() > 0;
    uint32_t now = millis();

    if (touched && !g_was_touched) {
        g_press_start = now;
        g_fired = false;
    }
    bool out = false;
    if (touched && !g_fired && (now - g_press_start) >= kHoldMs) {
        g_fired = true;
        out = true;
    }
    g_was_touched = touched;
    return out;
}

void redrawAfterModeChange() {
    if (g_mode == ModeManager::Mode::Config) {
        jarvis::hal::Display::drawConfigScreen();
    } else {
        // Returning to Normal: full repaint. setStatus(IDLE) alone
        // isn't enough — it early-returns when the FSM state didn't
        // change while paused, and even when it does fire it only
        // touches chrome + transcript (the reactor band and footer
        // would keep whatever the Config screen left on them).
        jarvis::hal::Display::drawHomeScreen();
    }
}
}  // namespace

void ModeManager::begin() {
    g_mode = Mode::Normal;
    g_last_activity_ms = 0;
    Serial.println("[Mode] Normal");
}

void ModeManager::tick() {
    // Mode toggle via long-press.
    if (detectLongPress()) {
        if (g_mode == Mode::Config) enterNormal();
        else                        enterConfig();
        return;  // skip remaining checks this tick — mode just changed
    }

    if (g_mode != Mode::Config) return;

    // Web UI requested exit?
    if (jarvis::net::CaptivePortal::exitRequested()) {
        jarvis::net::CaptivePortal::clearExitFlag();
        enterNormal();
        return;
    }

    // Inactivity auto-exit. CaptivePortal API hits update last_activity
    // via noteActivity(); a session with no interaction for kConfigIdle
    // returns to Normal so we don't broadcast an open AP forever.
    if (g_last_activity_ms && (millis() - g_last_activity_ms) > kConfigIdleTimeoutMs) {
        Serial.println("[Mode] Config idle timeout");
        enterNormal();
    }
}

ModeManager::Mode ModeManager::mode()     { return g_mode; }
bool              ModeManager::isConfig() { return g_mode == Mode::Config; }

void ModeManager::enterConfig() {
    if (g_mode == Mode::Config) return;
    Serial.println("[Mode] -> Config");
    jarvis::net::CaptivePortal::begin();
    g_mode = Mode::Config;
    g_last_activity_ms = millis();
    redrawAfterModeChange();
}

void ModeManager::enterNormal() {
    if (g_mode == Mode::Normal) return;
    Serial.println("[Mode] -> Normal");
    jarvis::net::CaptivePortal::end();
    g_mode = Mode::Normal;
    g_last_activity_ms = 0;

    // Re-establish STA connection. WiFiManager::begin() handles the
    // reconnect using the multi-slot creds in app::WiFiCreds.
    jarvis::net::WiFiManager::begin(20000);
    redrawAfterModeChange();
}

void ModeManager::noteActivity() {
    g_last_activity_ms = millis();
}

}  // namespace jarvis::app

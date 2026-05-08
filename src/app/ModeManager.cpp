#include "ModeManager.h"

#include <M5Unified.h>
#include <WiFi.h>

#include "../hal/Display.h"
#include "../net/CaptivePortal.h"
#include "../net/WiFiManager.h"
#include "NVSConfig.h"

namespace jarvis::app {

namespace {
constexpr uint32_t kConfigIdleTimeoutMs = 5UL * 60000;  // 5 minutes

// Audio confirmation when a long-press fires. Low frequency reads as a
// "thump" rather than a beep — short enough to feel like haptic feedback
// without being annoying. Fires before the mode switch so the user gets
// the cue even if WiFi/AP startup takes a moment.
constexpr uint32_t kPressTickHz = 80;
constexpr uint32_t kPressTickMs = 40;

ModeManager::Mode g_mode = ModeManager::Mode::Normal;
uint32_t          g_last_activity_ms = 0;

// Long-press detector state. `g_press_start == 0` means no active press;
// otherwise it's millis() at the moment the press began. `g_last_touch_at`
// is the most recent millis() where we saw a touch, used to bridge brief
// FT6336 sample dropouts via the slack window.
bool     g_fired         = false;
uint32_t g_press_start   = 0;
uint32_t g_last_touch_at = 0;

bool detectLongPress(uint32_t hold_ms, uint32_t slack_ms) {
    bool     touched = M5.Touch.getCount() > 0;
    uint32_t now     = millis();

    if (touched) {
        // Fresh press if we weren't tracking one, or if the gap since
        // the last touch sample exceeds slack (i.e. user really did
        // lift). Otherwise treat this sample as continuation of the
        // current press — bridges FT6336 dropouts and finger-waver.
        if (g_press_start == 0 || (now - g_last_touch_at) > slack_ms) {
            g_press_start = now;
            g_fired       = false;
        }
        g_last_touch_at = now;
    } else if (g_press_start && (now - g_last_touch_at) > slack_ms) {
        // Released long enough — clear state so the next press starts
        // a fresh timer.
        g_press_start = 0;
        g_fired       = false;
    }

    if (g_press_start && !g_fired && (now - g_press_start) >= hold_ms) {
        g_fired = true;
        return true;
    }
    return false;
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
    // Mode toggle via long-press. Threshold and slack come from NVS so
    // the user can tune responsiveness without a re-flash. Reading
    // every tick is fine — Preferences caches; the cost is a couple of
    // microseconds per call.
    uint32_t hold_ms  = (uint32_t)NVSConfig::getHoldMs();
    uint32_t slack_ms = (uint32_t)NVSConfig::getHoldSlack();
    if (detectLongPress(hold_ms, slack_ms)) {
        // Audio confirmation — fires before the mode switch so the
        // user gets the cue even though enterConfig()/enterNormal()
        // can take a moment (AP startup, STA reconnect).
        M5.Speaker.tone(kPressTickHz, kPressTickMs);
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

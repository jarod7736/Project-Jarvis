#include "ModeManager.h"

#include <M5Unified.h>
#include <WiFi.h>

#include "../hal/Display.h"
#include "../net/CaptivePortal.h"
#include "../net/WiFiManager.h"
#include "NVSConfig.h"
#include "SettingsScreen.h"

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

// Swipe-down-from-top detector state. Independent from the long-press
// detector so the two can coexist without sharing slack logic. Tracks the
// first-touch point of the current gesture and a "fired once" latch so we
// don't re-fire mid-drag if the user keeps dragging past the threshold.
//
// Trigger: press must start within the top kSwipeTopBandH px of the
// screen, then move down by ≥ kSwipeMinDy within kSwipeMaxMs. X is
// unconstrained — landing anywhere along the top edge is fine.
constexpr int      kSwipeTopBandH = 48;
constexpr int      kSwipeMinDy    = 70;
constexpr uint32_t kSwipeMaxMs    = 2500;

// Verbose serial diagnostics for the swipe gesture. Set to false once
// the gesture is reliable on hardware — for now we want to see every
// press, every in-band press, and every reason the detector chose not
// to fire. Cheap: Serial.print on touch events only, not every tick.
constexpr bool kSwipeLogVerbose = true;

bool     g_swipe_fired      = false;
int16_t  g_swipe_start_x    = 0;
int16_t  g_swipe_start_y    = 0;
uint32_t g_swipe_start_ms   = 0;
bool     g_swipe_in_band    = false;  // press started in the top band
int16_t  g_swipe_max_dy     = 0;      // peak Δy seen this gesture (for logs)

bool detectSwipeDownFromTop() {
    if (M5.Touch.getCount() == 0) {
        // Press ended (or never started this tick) — reset latches so the
        // next press starts a fresh gesture window. Log the outcome of any
        // gesture that started in the band so we can see WHY it didn't
        // fire (band hit but distance never reached, or time exceeded).
        if (kSwipeLogVerbose && g_swipe_in_band && !g_swipe_fired) {
            Serial.printf("[Swipe] released without firing: "
                          "start=(%d,%d) max_dy=%d elapsed=%lums "
                          "(needed dy>=%d within %lums)\n",
                          g_swipe_start_x, g_swipe_start_y,
                          (int)g_swipe_max_dy,
                          (unsigned long)(millis() - g_swipe_start_ms),
                          kSwipeMinDy, (unsigned long)kSwipeMaxMs);
        }
        g_swipe_fired   = false;
        g_swipe_in_band = false;
        g_swipe_max_dy  = 0;
        return false;
    }
    const auto& td = M5.Touch.getDetail();

    if (td.wasPressed()) {
        g_swipe_start_x  = td.x;
        g_swipe_start_y  = td.y;
        g_swipe_start_ms = millis();
        g_swipe_fired    = false;
        g_swipe_max_dy   = 0;
        g_swipe_in_band  = (td.y >= 0 && td.y < kSwipeTopBandH);
        if (kSwipeLogVerbose) {
            Serial.printf("[Swipe] press @ (%d,%d) %s top band (h=%d)\n",
                          (int)td.x, (int)td.y,
                          g_swipe_in_band ? "INSIDE" : "outside",
                          kSwipeTopBandH);
        }
    }

    if (g_swipe_fired || !g_swipe_in_band) return false;

    int dy = td.y - g_swipe_start_y;
    if (dy > g_swipe_max_dy) g_swipe_max_dy = dy;

    if ((millis() - g_swipe_start_ms) > kSwipeMaxMs) return false;
    if (dy < kSwipeMinDy)                            return false;

    g_swipe_fired = true;
    if (kSwipeLogVerbose) {
        Serial.printf("[Swipe] FIRED dy=%d elapsed=%lums\n",
                      dy, (unsigned long)(millis() - g_swipe_start_ms));
    }
    return true;
}

// Cancel the long-press timer if the finger has travelled this far from
// its initial position. Prevents swipes (e.g. the top-left swipe-down that
// opens the Settings screen) from incidentally tripping Config-mode entry
// when the user lingers at the bottom of the gesture.
constexpr int kLongPressMoveCancelPx = 25;

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

        // Movement cancel: if the finger has moved far enough from the
        // gesture's base point, treat this as a drag/swipe rather than
        // a hold and reset the press timer. distanceX/Y are measured
        // relative to the touch_detail_t's `base`, which is set at
        // wasPressed() — same anchor the swipe detector uses, so the
        // two detectors agree on what "moved" means.
        const auto& td = M5.Touch.getDetail();
        int dx = td.distanceX(); if (dx < 0) dx = -dx;
        int dy = td.distanceY(); if (dy < 0) dy = -dy;
        if (dx > kLongPressMoveCancelPx || dy > kLongPressMoveCancelPx) {
            g_press_start = 0;
            g_fired       = false;
            return false;
        }
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
    } else if (g_mode == ModeManager::Mode::Settings) {
        // SettingsScreen owns the display for the duration of the mode;
        // it does its own full paint on enter().
        SettingsScreen::enter();
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
    // Settings mode: touch belongs to the SettingsScreen widgets, not
    // to the long-press or swipe detectors. Pumping those here would
    // either fire spurious mode toggles mid-slider-drag or steal the
    // touch from the screen's hit-tests. SettingsScreen owns the exit
    // path (its close button calls exitSettings()).
    if (g_mode == Mode::Settings) {
        SettingsScreen::tick();
        return;
    }

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

    // Swipe-down-from-top → Settings. Only meaningful from Normal:
    // Config has its own touch model (long-press exits) and Settings
    // returned early above.
    if (g_mode == Mode::Normal && detectSwipeDownFromTop()) {
        M5.Speaker.tone(kPressTickHz, kPressTickMs);
        enterSettings();
        return;
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

ModeManager::Mode ModeManager::mode()       { return g_mode; }
bool              ModeManager::isConfig()   { return g_mode == Mode::Config; }
bool              ModeManager::isSettings() { return g_mode == Mode::Settings; }

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

void ModeManager::enterSettings() {
    if (g_mode == Mode::Settings) return;
    Serial.println("[Mode] -> Settings");
    // If we're somehow coming from Config, tear the AP down first so
    // the captive portal doesn't keep advertising while the slider
    // screen is up. (In practice the swipe gesture only fires from
    // Normal, but covering the case keeps the transition symmetric.)
    if (g_mode == Mode::Config) {
        jarvis::net::CaptivePortal::end();
    }
    g_mode = Mode::Settings;
    g_last_activity_ms = 0;
    redrawAfterModeChange();
}

void ModeManager::exitSettings() {
    if (g_mode != Mode::Settings) return;
    Serial.println("[Mode] Settings -> Normal");
    SettingsScreen::exit();
    g_mode = Mode::Normal;
    g_last_activity_ms = 0;
    // Full home-screen repaint — SettingsScreen owned every pixel while
    // up, so the reactor / chrome / footer all need to be rebuilt.
    jarvis::hal::Display::drawHomeScreen();
}

void ModeManager::noteActivity() {
    g_last_activity_ms = millis();
}

}  // namespace jarvis::app

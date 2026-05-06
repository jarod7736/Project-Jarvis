// Project Jarvis — Phase 2 (custom voice loop) + Phase 3 starter (WiFi/NVS).
//
// Replaces the bundled M5ModuleLLM_VoiceAssistant preset (Phase 1) with a
// hand-wired chain that explicitly sends audio.work and uses a 60s wait for
// melotts.setup — see PLAN.md Phase 1 retro for why both are necessary on
// current StackFlow firmware.
//
// loop() does just two things: pump the HAL's UART poll, then tick the FSM.
// All callbacks set flags only — see hal/LLMModule.h and app/state_machine.h.

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_task_wdt.h>

#include "app/IntentRouter.h"
#include "app/NVSConfig.h"
#include "app/state_machine.h"
#include "config.h"
#include "hal/AudioPlayer.h"
#include "hal/Display.h"
#include "hal/LLMModule.h"
#include "hal/SdLogger.h"
#include "net/HAClient.h"
#include "net/LLMClient.h"
#include "net/WiFiManager.h"

namespace {
jarvis::hal::LLMModule g_module;

// Footer state — track last-rendered tier/rssi so we only redraw when
// they change, avoiding flicker at the configured refresh cadence.
jarvis::net::ConnectivityTier g_last_tier = jarvis::net::ConnectivityTier::OFFLINE;
int                            g_last_rssi = 0;
uint32_t                       g_last_footer_ms = 0;

// Battery indicator state — polled from AXP2101 via M5.Power. Polling
// budget is set low because the IC is on i2c and the call is cheap, but
// we still throttle to avoid pointless redraws. Flips can happen at any
// time (USB plug/unplug) so unlike the footer we don't gate on IDLE.
int      g_last_batt_level    = -2;  // sentinel for "never rendered"
bool     g_last_batt_charging = false;
uint32_t g_last_batt_ms       = 0;
constexpr uint32_t kBatteryPollMs = 5000;

void refreshBattery() {
    if (millis() - g_last_batt_ms < kBatteryPollMs) return;
    g_last_batt_ms = millis();
    int  level     = M5.Power.getBatteryLevel();    // -1 if unknown, else 0..100
    bool charging  = M5.Power.isCharging();
    int  v_mv      = M5.Power.getBatteryVoltage();  // mV; useful when level=-1
    int  vbus_mv   = M5.Power.getVBUSVoltage();     // -1 if unsupported

    // If the SOC counter hasn't initialized yet (returns -1) but we have a
    // valid voltage reading, fall back to a crude voltage→percent map. The
    // AXP2101 fuel gauge needs a few seconds of i2c traffic to produce a
    // real SOC, and on CoreS3 it sometimes wedges at -1 indefinitely after
    // a USB-powered boot. 4.20V = 100%, 3.30V = 0%, linear in between.
    if (level < 0 && v_mv >= 3300) {
        if (v_mv >= 4200) level = 100;
        else              level = ((v_mv - 3300) * 100) / (4200 - 3300);
    }

    // AXP2101 reports isCharging()=false at 100% because it stops actively
    // pushing current into a full cell — but the device is still plugged
    // in and the user expects a "powered" indicator. Treat VBUS > 4.5V as
    // "on USB" and force the bolt on. -1 vbus = the chip can't tell us,
    // so we trust isCharging() alone in that case.
    bool on_usb = (vbus_mv >= 4500);
    bool show_bolt = charging || on_usb;

    bool changed = (level != g_last_batt_level) || (show_bolt != g_last_batt_charging);
    if (changed) {
        Serial.printf("[Power] level=%d charging=%d v=%dmV vbus=%dmV bolt=%d\n",
                      level, (int)charging, v_mv, vbus_mv, (int)show_bolt);
    }
    if (!changed) return;
    g_last_batt_level    = level;
    g_last_batt_charging = show_bolt;
    jarvis::hal::Display::updateBattery(level, show_bolt);
}

// Refresh the footer's tier/rssi line. Cheap when tier is cached; the
// underlying probe can take 2-5s so we only call this from IDLE so it
// doesn't stall a voice cycle.
void refreshFooterIfIdle() {
    using jarvis::app::currentState;
    using jarvis::hal::DeviceState;
    using jarvis::net::WiFiManager;

    if (currentState() != DeviceState::IDLE) return;
    if (millis() - g_last_footer_ms < 1000) return;  // throttle render

    auto tier = WiFiManager::getConnectivityTier();
    int  rssi = WiFiManager::isConnected() ? WiFiManager::getRSSI() : 0;
    if (tier == g_last_tier && rssi == g_last_rssi) {
        g_last_footer_ms = millis();
        return;
    }
    jarvis::hal::Display::updateFooter(jarvis::net::tierName(tier), rssi);
    g_last_tier = tier;
    g_last_rssi = rssi;
    g_last_footer_ms = millis();
}
}  // namespace

void setup() {
    M5.begin();
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("=== Project Jarvis — Phase 2 voice loop ===");

    // Power-stack sanity: if the AXP2101 isn't being driven by M5Unified
    // (board mis-detected, i2c bus busy, etc.) all our M5.Power calls
    // silently return defaults. Log once at boot so it's obvious.
    Serial.printf("[Power] board=%d batt_level=%d v_mv=%d charging=%d\n",
                  (int)M5.getBoard(),
                  (int)M5.Power.getBatteryLevel(),
                  (int)M5.Power.getBatteryVoltage(),
                  (int)M5.Power.isCharging());

    jarvis::hal::Display::begin();

    // SD logger: best-effort. Mount failure (no card / unformatted) is
    // logged once and silently ignored thereafter — voice loop still runs.
    // Uses the global SPI instance shared with M5GFX (per CoreS3 sdcard
    // example) — a separate SPIClass(HSPI) freezes the display.
    jarvis::hal::SdLogger::begin();

    // Phase 7 cloud-TTS playback: bring up M5.Speaker + ESP8266Audio MP3
    // decoder. Always init even if no cloud key is provisioned — the
    // audio chain is the same hardware path used by future SFX (chime
    // on wake, error tones, etc.). LLMModule::speak() decides at call
    // time whether to route through here or melotts.
    jarvis::hal::AudioPlayer::begin();

    // Phase 3: WiFi bring-up first. NVS-backed creds; first-run provisioning
    // over USB Serial JSON. OFFLINE on failure — voice loop still runs so the
    // hardware path can be exercised without the network.
    Serial.print("[WiFi] connecting...");
    bool wifi_ok = jarvis::net::WiFiManager::begin(20000);
    if (wifi_ok) {
        Serial.printf(" OK  ip=%s rssi=%d\n",
                      jarvis::net::WiFiManager::getIP().c_str(),
                      jarvis::net::WiFiManager::getRSSI());
    } else {
        Serial.println(" OFFLINE");
    }
    // Initial footer paint — refreshFooterIfIdle() will replace this with
    // the tier-tagged version once getConnectivityTier() probes complete.
    jarvis::hal::Display::updateFooter("...", wifi_ok ? jarvis::net::WiFiManager::getRSSI() : 0);

    // Phase 4/6: prompt for credentials if any of the optional services is
    // unconfigured. Short window (30s). Bag-of-keys JSON: any combination
    // of ssid, pass, ha_token, ha_host, oc_key, oc_host.
    bool need_ha = (jarvis::NVSConfig::getHaToken().length() == 0);
    bool need_oc = (jarvis::NVSConfig::getOcKey().length()   == 0);
    if (wifi_ok && (need_ha || need_oc)) {
        Serial.printf("[PROV] Missing creds: ha_token=%s oc_key=%s\n",
                      need_ha ? "MISSING" : "set",
                      need_oc ? "MISSING" : "set");
        Serial.println("[PROV] Send JSON now, or skip and those services error.");
        jarvis::NVSConfig::provisionFromSerial(30000);
    }
    Serial.printf("[HA] configured=%s host=%s\n",
                  jarvis::net::HAClient::isConfigured() ? "yes" : "no",
                  jarvis::NVSConfig::getHaHost().c_str());
    Serial.printf("[OC] configured=%s host=%s\n",
                  jarvis::net::LLMClient::isConfigured() ? "yes" : "no",
                  jarvis::NVSConfig::getOcHost().c_str());

    // M5Bus UART: 115200 8N1. Pins resolved at runtime via Port C — they
    // differ across CoreS3 revisions, don't hardcode.
    int rxd = M5.getPin(m5::pin_name_t::port_c_rxd);
    int txd = M5.getPin(m5::pin_name_t::port_c_txd);
    Serial.printf("[UART] Port C: RX=%d TX=%d @ 115200 8N1\n", rxd, txd);
    Serial2.begin(115200, SERIAL_8N1, rxd, txd);

    if (!g_module.begin(&Serial2)) {
        Serial.printf("[FATAL] LLMModule.begin failed: %s\n",
                      g_module.getLastError().c_str());
        jarvis::hal::Display::setStatus(jarvis::hal::DeviceState::ERROR);
        // Halt — Phase 4+ retries; Phase 2 just stops and waits for reboot.
        while (true) delay(1000);
    }

    jarvis::app::intentRouterBegin(&g_module);
    jarvis::app::stateMachineBegin(&g_module);

    // Wire AudioPlayer's "track ended" event back into LLMModule so the
    // FSM's existing on_speak_done_ callback (registered by
    // stateMachineBegin) gets fired when cloud TTS finishes — same way
    // melotts' millis()-based timer fires it for the local path.
    jarvis::hal::AudioPlayer::setOnPlayDone([]() {
        g_module.finishSpeaking();
    });

    Serial.printf("[READY] Say \"%s\" to wake.\n", jarvis::config::kWakeWord);

    // Hardware watchdog: panics + reboots if loop() goes silent for
    // longer than kWatchdogTimeoutSec. The single source of truth for
    // "we're alive" is reaching the top of loop() — an HTTP/UART hang
    // that doesn't return control trips the dog. Subscribing the
    // current task (loop task) is enough; freertos drives the rest.
    //
    // espressif32@6.13.0 ships Arduino-ESP32 2.x / IDF 4.x — old 2-arg
    // init signature. IDF 5.x's struct form (esp_task_wdt_config_t) is
    // not available until Arduino-ESP32 3.x.
    esp_task_wdt_init(jarvis::config::kWatchdogTimeoutSec, /*panic=*/true);
    esp_task_wdt_add(nullptr);   // subscribe the current (loop) task
    Serial.printf("[WDT] armed: timeout=%us\n",
                  (unsigned)jarvis::config::kWatchdogTimeoutSec);
}

void loop() {
    // Feed the watchdog at the top of every iteration. Anything below
    // that hangs longer than kWatchdogTimeoutSec triggers a panic-reboot.
    esp_task_wdt_reset();

    g_module.update();
    jarvis::hal::AudioPlayer::tick();
    jarvis::app::tickStateMachine();
    refreshFooterIfIdle();
    refreshBattery();
}

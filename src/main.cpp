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

#include "app/state_machine.h"
#include "config.h"
#include "hal/Display.h"
#include "hal/LLMModule.h"
#include "net/WiFiManager.h"

namespace {
jarvis::hal::LLMModule g_module;
}  // namespace

void setup() {
    M5.begin();
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("=== Project Jarvis — Phase 2 voice loop ===");

    jarvis::hal::Display::begin();

    // Phase 3: WiFi bring-up first. NVS-backed creds; first-run provisioning
    // over USB Serial JSON. OFFLINE on failure — voice loop still runs so the
    // hardware path can be exercised without the network.
    Serial.print("[WiFi] connecting...");
    bool wifi_ok = jarvis::net::WiFiManager::begin(20000);
    if (wifi_ok) {
        Serial.printf(" OK  ip=%s rssi=%d\n",
                      jarvis::net::WiFiManager::getIP().c_str(),
                      jarvis::net::WiFiManager::getRSSI());
        jarvis::hal::Display::updateFooter(
            "ON", jarvis::net::WiFiManager::getRSSI());
    } else {
        Serial.println(" OFFLINE");
        jarvis::hal::Display::updateFooter("OFF", 0);
    }

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

    jarvis::app::stateMachineBegin(&g_module);

    Serial.printf("[READY] Say \"%s\" to wake.\n", jarvis::config::kWakeWord);
}

void loop() {
    g_module.update();
    jarvis::app::tickStateMachine();
}

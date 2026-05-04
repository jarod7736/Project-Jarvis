#pragma once

// NVSConfig — thin wrapper over Preferences for the "jarvis" NVS namespace.
//
// Per CLAUDE.md: credentials live in NVS, namespace "jarvis", keys ≤15 chars.
// Never hardcode WiFi/HA/OpenClaw secrets. First-run provisioning is via USB
// Serial JSON when `wifi0_ssid` is empty.
//
// This is the seed of `app/NVSConfig` that PLAN.md Phase 3 prescribes — only
// the WiFi-slot-0 keys are wired up here; HA, OpenClaw, hotspot, and FW URL
// keys will land in their respective phases.

#include <Arduino.h>

namespace jarvis {

class NVSConfig {
public:
    static String getWiFi0SSID();
    static String getWiFi0Pass();
    static bool   setWiFi0(const String& ssid, const String& pass);

    // Blocks reading USB Serial up to `timeoutMs` for a single line containing
    // {"ssid":"<ssid>","pass":"<password>"}. On success writes both keys to NVS
    // and returns true. The credentials never re-enter Serial output — only the
    // SSID name is echoed for confirmation, never the password.
    static bool provisionWiFiFromSerial(uint32_t timeoutMs = 180000);
};

}  // namespace jarvis

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

    // Home Assistant. Token never re-enters Serial output (security parity
    // with the WiFi password). `ha_host` falls back to config::kHaHostDefault
    // when unset.
    static String getHaToken();
    static bool   setHaToken(const String& token);
    static String getHaHost();
    static bool   setHaHost(const String& host);

    // OpenClaw (Phase 6). Same security pattern as HA; `oc_host` falls back
    // to config::kOpenclawHostDefault.
    static String getOcKey();
    static bool   setOcKey(const String& key);
    static String getOcHost();
    static bool   setOcHost(const String& host);

    // Blocks reading USB Serial up to `timeoutMs` for a single JSON line.
    // The provisioning JSON is a bag of optional fields:
    //   {"ssid":"...", "pass":"...", "ha_token":"...", "ha_host":"..."}
    // Each present key is written to NVS; absent keys are left alone. The
    // WiFi-only flow that enters from begin() requires `ssid`+`pass` —
    // see `provisionWiFiFromSerial()` below for that variant. This generic
    // entry point is what Phase 4+ provisioning calls.
    static bool provisionFromSerial(uint32_t timeoutMs = 180000);

    // Phase 3 entry point — same wire format, but rejects input that's
    // missing `ssid`. Kept for the WiFi-cred-required boot path.
    static bool provisionWiFiFromSerial(uint32_t timeoutMs = 180000);
};

}  // namespace jarvis

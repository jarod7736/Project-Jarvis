#pragma once

// WiFiManager — Phase 3 WiFi bring-up.
//
// Per CLAUDE.md: per-AP WiFi timeout via WiFiMulti.run(500) for fast failover.
// Connectivity-tier classification (LAN / TAILSCALE / HOTSPOT_ONLY / OFFLINE)
// is intentionally deferred until Phase 4 endpoints (HA host, OpenClaw host)
// are available — this seed only covers connect + status.

#include <Arduino.h>

namespace jarvis::net {

class WiFiManager {
public:
    // Read NVS-stored creds (slot 0). If absent, run USB-Serial provisioning,
    // then attempt WiFi.connect via WiFiMulti for at most `connectTimeoutMs`.
    // Non-blocking past timeout — returns false rather than hanging the boot.
    static bool begin(uint32_t connectTimeoutMs = 20000);

    static bool   isConnected();
    static String getIP();
    static int    getRSSI();
};

}  // namespace jarvis::net

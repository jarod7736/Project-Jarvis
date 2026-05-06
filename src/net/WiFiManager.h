#pragma once

// WiFiManager — Phase 3 WiFi bring-up + connectivity-tier classification.
//
// Per CLAUDE.md: per-AP WiFi timeout via WiFiMulti.run(500) for fast failover.
//
// Connectivity-tier classification determines which backends are reachable
// from the current network. Routing code (Phase 5+) branches on this.

#include <Arduino.h>

namespace jarvis::net {

enum class ConnectivityTier {
    OFFLINE       = 0,  // no WiFi association
    HOTSPOT_ONLY  = 1,  // WiFi up, but neither HA nor OpenClaw reachable
    TAILSCALE     = 2,  // OpenClaw reachable but not HA
    LAN           = 3,  // HA reachable (the home-network case — covers both)
};

const char* tierName(ConnectivityTier t);   // "OFFLINE" / "HOT" / "TS" / "LAN"

class WiFiManager {
public:
    // Read NVS-stored creds (slot 0). If absent, run USB-Serial provisioning,
    // then attempt WiFi.connect via WiFiMulti for at most `connectTimeoutMs`.
    // Non-blocking past timeout — returns false rather than hanging the boot.
    static bool begin(uint32_t connectTimeoutMs = 20000);

    static bool   isConnected();
    static String getIP();
    static int    getRSSI();

    // Returns the cached tier if last check was within kTierRecheckMs;
    // otherwise probes HA + OpenClaw and updates the cache. Probes are
    // synchronous TCP connects — call from loop()-driven code, never from a
    // callback. End-to-end probe time is bounded by kHaProbeMs + kOpenclawProbeMs.
    static ConnectivityTier getConnectivityTier();

    // Force a re-probe on the next getConnectivityTier() call. Use after
    // events that likely change connectivity (e.g. WiFi reconnect).
    static void invalidateTierCache();

    // Single host:port reachability check. TCP connect with caller-supplied
    // timeout, no I/O after connect. Used internally by getConnectivityTier
    // but exposed for any future endpoint health-check needs.
    static bool isReachable(const char* host, uint16_t port, uint32_t timeoutMs);
};

}  // namespace jarvis::net

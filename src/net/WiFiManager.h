#pragma once

// WiFiManager — Phase 3 WiFi bring-up + connectivity-tier classification.
//
// Saved networks are tried in slot order (slot 0 = highest priority).
// Each slot gets a per-slot connect budget; on failure we move to the
// next slot. This replaces the prior WiFiMulti strongest-RSSI behavior
// so the captive-portal "priority N" labels are authoritative.
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
    // Iterate saved networks in slot-priority order, attempting each
    // until one connects or `connectTimeoutMs` elapses. If no slots
    // are saved, runs USB-Serial provisioning first. Non-blocking past
    // the budget — returns false rather than hanging the boot.
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

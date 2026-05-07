#pragma once

// WiFiCreds — multi-network credential store backed by NVS.
//
// Sits alongside NVSConfig (namespace "jarvis") in a SEPARATE namespace
// ("wifi") so that wiping WiFi creds from the captive-portal UI doesn't
// touch HA tokens / OpenClaw keys / etc., and vice versa.
//
// Holds up to kMaxNetworks slots, indexed by priority (0 == most recently
// added / preferred). add() inserts at the front and pushes older entries
// down; if the table is full, the oldest (highest-index) entry is evicted.
// This MRU policy lines up with the captive-portal UX: the network you
// just typed a password for should be the one Jarvis tries first on next
// boot.
//
// The legacy single-slot keys (`wifi0_ssid`/`wifi0_pass` in NVSConfig) are
// migrated lazily — the first call to load() that finds an empty "wifi"
// namespace but a non-empty `wifi0_ssid` in "jarvis" copies it over so
// existing devices don't lose their saved network on first run with this
// code. After migration, NVSConfig::getWiFi0SSID() still reads the old
// key (we don't delete it) — WiFiManager is updated in the same change to
// prefer this store when it's populated.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace jarvis::app {

class WiFiCreds {
public:
    static constexpr size_t kMaxNetworks = 5;

    struct Network {
        String ssid;
        String password;
    };

    // Read all stored networks into `out` (caller-allocated kMaxNetworks
    // array). `count` is set to the actual number stored. Performs the
    // one-time migration from the legacy single-slot keys if the multi-
    // slot store is empty and the legacy SSID is present.
    static void load(Network out[kMaxNetworks], size_t& count);

    // Add or update a network. If `ssid` is already present, its password
    // is overwritten and it is moved to the front (priority 0). Otherwise
    // a new entry is inserted at the front; the oldest is evicted when
    // the table is full. Empty `ssid` is rejected.
    static bool add(const String& ssid, const String& password);

    // Remove the entry with this SSID. No-op if not present.
    static bool remove(const String& ssid);

    // Append the saved networks to `arr` as JSON objects:
    //   [{"ssid": "Home", "priority": 0}, ...]
    // Passwords are NEVER serialized — the captive-portal UI shows
    // saved networks for management only.
    static void list(JsonArray arr);
};

}  // namespace jarvis::app

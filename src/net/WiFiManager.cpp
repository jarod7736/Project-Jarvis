#include "WiFiManager.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiMulti.h>

#include "../app/NVSConfig.h"
#include "../config.h"

namespace jarvis::net {

static WiFiMulti g_wifi;

// Tier cache. Re-probed at most every kTierRecheckMs. Initialized to OFFLINE
// so any call before begin() reports the correct state.
static ConnectivityTier g_tier_cached      = ConnectivityTier::OFFLINE;
static uint32_t         g_tier_checked_at  = 0;
static bool             g_tier_have_sample = false;

static const char* authName(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
        default:                        return "?";
    }
}

// Multi-pass passive scan of all 2.4GHz channels. Passive (listens for beacons)
// is more reliable than active probe-request scans for APs that don't respond
// to wildcard probes. 500ms dwell × ~13 channels × 3 passes ≈ 20s total — slow
// but exhaustive. Used as a provisioning aid: confirms whether a target SSID
// is reachable to the ESP32-S3 (which can only see 2.4GHz, no 5GHz, no DFS).
static void scanAndPrintSSIDs(uint8_t maxPerPass = 20) {
    WiFi.mode(WIFI_STA);
    Serial.println("[WIFI] Scanning all 2.4GHz channels (3 passive passes, 500ms/ch)...");
    Serial.printf ("[WIFI] STA MAC=%s\n", WiFi.macAddress().c_str());

    for (int pass = 1; pass <= 3; ++pass) {
        int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true,
                                  /*passive=*/true, /*max_ms_per_chan=*/500);
        if (n < 0) {
            Serial.printf("[WIFI] Pass %d: scan error %d\n", pass, n);
        } else if (n == 0) {
            Serial.printf("[WIFI] Pass %d: 0 networks found\n", pass);
        } else {
            Serial.printf("[WIFI] Pass %d: %d networks found:\n", pass, n);
            int shown = 0;
            for (int i = 0; i < n && shown < maxPerPass; ++i) {
                String s = WiFi.SSID(i);
                Serial.printf("[WIFI]   rssi=%4d ch=%2d auth=%-7s \"%s\"\n",
                              WiFi.RSSI(i),
                              WiFi.channel(i),
                              authName(WiFi.encryptionType(i)),
                              s.length() ? s.c_str() : "<hidden>");
                ++shown;
            }
        }
        WiFi.scanDelete();
        delay(200);
    }
}

// Single connect attempt against the currently-loaded creds.
static bool tryConnect(const String& ssid, const String& pass, uint32_t connectTimeoutMs) {
    WiFi.mode(WIFI_STA);
    g_wifi.addAP(ssid.c_str(), pass.c_str());

    Serial.printf("[WIFI] Connecting to \"%s\"", ssid.c_str());
    uint32_t t0 = millis();
    while (g_wifi.run(500) != WL_CONNECTED && millis() - t0 < connectTimeoutMs) {
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] OK ip=%s rssi=%d ch=%d mac=%s\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.RSSI(),
                      WiFi.channel(),
                      WiFi.macAddress().c_str());
        return true;
    }
    Serial.printf("[WIFI] FAILED to connect within %lums (status=%d)\n",
                  (unsigned long)connectTimeoutMs,
                  (int)WiFi.status());
    return false;
}

bool WiFiManager::begin(uint32_t connectTimeoutMs) {
    String ssid = jarvis::NVSConfig::getWiFi0SSID();
    String pass = jarvis::NVSConfig::getWiFi0Pass();

    // First-run provisioning: dump scan results so the user can see exactly
    // what SSIDs are visible to the chip before they paste a JSON blob.
    if (ssid.length() == 0) {
        Serial.println("[WIFI] No saved credentials — entering provisioning.");
        scanAndPrintSSIDs();
        if (!jarvis::NVSConfig::provisionWiFiFromSerial()) {
            Serial.println("[WIFI] Provisioning failed; staying offline.");
            return false;
        }
        ssid = jarvis::NVSConfig::getWiFi0SSID();
        pass = jarvis::NVSConfig::getWiFi0Pass();
    }

    if (tryConnect(ssid, pass, connectTimeoutMs)) return true;

    // Connect failed against saved creds. Show what we DID see, then offer a
    // reprovisioning window so the user can correct a typo / wrong-band SSID
    // without having to re-flash to clear NVS.
    scanAndPrintSSIDs();
    Serial.println("[WIFI] Saved credentials did not connect. Send a fresh JSON to overwrite, or wait to stay offline.");
    if (!jarvis::NVSConfig::provisionWiFiFromSerial()) {
        Serial.println("[WIFI] No reprovisioning JSON received; staying offline.");
        return false;
    }
    ssid = jarvis::NVSConfig::getWiFi0SSID();
    pass = jarvis::NVSConfig::getWiFi0Pass();
    return tryConnect(ssid, pass, connectTimeoutMs);
}

bool   WiFiManager::isConnected() { return WiFi.status() == WL_CONNECTED; }
String WiFiManager::getIP()       { return WiFi.localIP().toString(); }
int    WiFiManager::getRSSI()     { return WiFi.RSSI(); }

const char* tierName(ConnectivityTier t) {
    switch (t) {
        case ConnectivityTier::LAN:          return "LAN";
        case ConnectivityTier::TAILSCALE:    return "TS";
        case ConnectivityTier::HOTSPOT_ONLY: return "HOT";
        case ConnectivityTier::OFFLINE:      return "OFF";
    }
    return "?";
}

bool WiFiManager::isReachable(const char* host, uint16_t port, uint32_t timeoutMs) {
    WiFiClient c;
    // ESP32's WiFiClient::connect(host, port, timeoutMs) is in milliseconds
    // despite the bare integer signature. Treat the result as definitive —
    // if it fails, the host is not reachable on TCP at this port.
    bool ok = c.connect(host, port, timeoutMs);
    c.stop();
    return ok;
}

void WiFiManager::invalidateTierCache() {
    g_tier_have_sample = false;
}

ConnectivityTier WiFiManager::getConnectivityTier() {
    // Cache hit?
    if (g_tier_have_sample &&
        (millis() - g_tier_checked_at) < jarvis::config::kTierRecheckMs) {
        return g_tier_cached;
    }

    if (WiFi.status() != WL_CONNECTED) {
        g_tier_cached      = ConnectivityTier::OFFLINE;
        g_tier_checked_at  = millis();
        g_tier_have_sample = true;
        return g_tier_cached;
    }

    // Probe both backends so the tier reflects what's actually reachable.
    // Originally we short-circuited on HA reachable → LAN, but that hid
    // OpenClaw failures (e.g. CoreS3 not on Tailscale → can't resolve
    // *.ts.net) when HA cloud was up. The router branches on this:
    // local_llm/claude calls require OC; HA dispatch requires HA. Routing
    // can fail silently if the tier reports "ok" without the right
    // backend actually being reachable.
    bool ha_ok = isReachable(jarvis::config::kHaHostDefault,
                             jarvis::config::kHaPortDefault,
                             jarvis::config::kHaProbeMs);
    bool oc_ok = isReachable(jarvis::config::kOpenclawProbeHost,
                             jarvis::config::kOpenclawPortDefault,
                             jarvis::config::kOpenclawProbeMs);
    Serial.printf("[WIFI] probe: HA=%s OC=%s\n",
                  ha_ok ? "ok" : "down",
                  oc_ok ? "ok" : "down");
    if (ha_ok && oc_ok) {
        g_tier_cached = ConnectivityTier::LAN;          // full mesh
    } else if (oc_ok) {
        g_tier_cached = ConnectivityTier::TAILSCALE;    // OC only
    } else {
        // HA may be up via cloud, but without OC the LLM intents have
        // nowhere to go. Report HOTSPOT_ONLY so the router falls back
        // gracefully.
        g_tier_cached = ConnectivityTier::HOTSPOT_ONLY;
    }

    g_tier_checked_at  = millis();
    g_tier_have_sample = true;
    Serial.printf("[WIFI] tier=%s rssi=%d\n", tierName(g_tier_cached), WiFi.RSSI());
    return g_tier_cached;
}

}  // namespace jarvis::net

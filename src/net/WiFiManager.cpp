#include "WiFiManager.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <time.h>

#include "../app/NVSConfig.h"
#include "../app/WiFiCreds.h"
#include "../config.h"

namespace jarvis::net {

// Per-slot connect budget for connectInSlotOrder(). 8 s gives the
// usual WPA2 handshake + DHCP a clean shot on a healthy AP without
// burning the whole connectTimeoutMs on a single failed slot. Tunable
// if a slow AP needs more.
static constexpr uint32_t kPerSlotTimeoutMs = 8000;

// Kick off an SNTP sync against the configured NTP server with the
// configured POSIX timezone. configTime() is non-blocking — the IDF's
// SNTP client runs the actual exchange in the background and updates
// the system clock when a response arrives (typically within 1–2 s on
// a healthy network). getLocalTime() will start returning true once
// the first sync completes.
//
// Idempotent: safe to call on every reconnect. configTime() reinit's
// the SNTP state machine without leaking handles.
static void kickNtpSync() {
    // configTzTime sets TZ atomically with the SNTP config. Using
    // setenv()+tzset()+configTime() separately races: configTime can
    // reset TZ to UTC internally on some Arduino-ESP32 versions, so
    // getLocalTime() returns UTC despite the setenv.
    configTzTime(jarvis::config::kTimezoneDefault,
                 jarvis::config::kNtpServer);
    Serial.printf("[WIFI] NTP sync requested (server=%s tz=%s)\n",
                  jarvis::config::kNtpServer,
                  jarvis::config::kTimezoneDefault);
}

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

// Iterate saved networks in slot order (slot 0 = highest priority).
// Each slot gets up to kPerSlotTimeoutMs to associate; on failure we
// move to the next slot. Total bounded by connectTimeoutMs so a long
// list of failing APs can't run past the caller's budget.
//
// Replaces the prior WiFiMulti.run() flow, which selected by strongest
// RSSI. RSSI-based selection meant a noisy iPhone hotspot in the same
// room would always beat the home router across the house, regardless
// of which slot the user marked "priority 1" in the captive portal.
// True slot-order priority makes that label do what it says.
//
// Runtime auto-reconnect (after a mid-session drop) still goes back to
// whichever AP the radio last associated with — the ESP32 driver
// handles that itself. So this function only governs boot-time and
// post-provision selection. That's the right scope for "priority."
static bool connectInSlotOrder(uint32_t connectTimeoutMs) {
    jarvis::app::WiFiCreds::Network nets[jarvis::app::WiFiCreds::kMaxNetworks];
    size_t count = 0;
    jarvis::app::WiFiCreds::load(nets, count);
    if (count == 0) return false;

    WiFi.mode(WIFI_STA);
    const uint32_t t_start = millis();

    for (size_t i = 0; i < count; ++i) {
        const auto& net = nets[i];
        if (net.ssid.isEmpty()) continue;

        const uint32_t elapsed = millis() - t_start;
        if (elapsed >= connectTimeoutMs) {
            Serial.printf("[WIFI] total budget %lums exhausted before slot %u\n",
                          (unsigned long)connectTimeoutMs, (unsigned)i);
            return false;
        }
        const uint32_t remaining = connectTimeoutMs - elapsed;
        const uint32_t slot_budget =
            (kPerSlotTimeoutMs < remaining) ? kPerSlotTimeoutMs : remaining;

        Serial.printf("[WIFI] slot %u \"%s\" (budget=%lums)",
                      (unsigned)i, net.ssid.c_str(),
                      (unsigned long)slot_budget);

        // Drop any partial association from a previous slot before a
        // clean attempt. eraseap=true clears the cached SSID so the
        // driver doesn't try to resume the previous attempt.
        WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/true);
        delay(100);

        WiFi.begin(net.ssid.c_str(), net.password.c_str());

        const uint32_t t_slot = millis();
        while (WiFi.status() != WL_CONNECTED &&
               millis() - t_slot < slot_budget) {
            delay(200);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] OK ssid=\"%s\" ip=%s rssi=%d ch=%d slot=%u mac=%s\n",
                          WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str(),
                          WiFi.RSSI(),
                          WiFi.channel(),
                          (unsigned)i,
                          WiFi.macAddress().c_str());
            kickNtpSync();
            return true;
        }

        Serial.printf("[WIFI] slot %u \"%s\" failed (status=%d), trying next\n",
                      (unsigned)i, net.ssid.c_str(), (int)WiFi.status());
    }

    Serial.printf("[WIFI] all %u slots exhausted\n", (unsigned)count);
    return false;
}

bool WiFiManager::begin(uint32_t connectTimeoutMs) {
    // Multi-network store ("wifi" namespace) holds up to 5 saved
    // networks. WiFiCreds::load() lazy-migrates the legacy single-slot
    // wifi0_ssid into slot 0 on first read, so existing devices upgrade
    // transparently. We don't register them with anything up front —
    // connectInSlotOrder() loads them itself and iterates by slot.
    {
        jarvis::app::WiFiCreds::Network nets[jarvis::app::WiFiCreds::kMaxNetworks];
        size_t count = 0;
        jarvis::app::WiFiCreds::load(nets, count);
        for (size_t i = 0; i < count; ++i) {
            Serial.printf("[WIFI] saved[%u] \"%s\"\n",
                          (unsigned)i, nets[i].ssid.c_str());
        }
        if (count == 0) {
            // First-run provisioning. Dump scan results so the user
            // can see what SSIDs are visible before pasting a JSON
            // blob.
            Serial.println("[WIFI] No saved credentials — entering provisioning.");
            scanAndPrintSSIDs();
            if (!jarvis::NVSConfig::provisionWiFiFromSerial()) {
                Serial.println("[WIFI] Provisioning failed; staying offline.");
                return false;
            }
            // Mirror the freshly-provisioned slot into the multi-store
            // so future boots and the captive-portal UI see it.
            String ssid = jarvis::NVSConfig::getWiFi0SSID();
            String pass = jarvis::NVSConfig::getWiFi0Pass();
            if (ssid.length()) jarvis::app::WiFiCreds::add(ssid, pass);
        }
    }

    if (connectInSlotOrder(connectTimeoutMs)) {
        return true;
    }

    // None of the saved networks were reachable. Show what we DID see
    // and offer a reprovisioning window — the new SSID gets pushed to
    // the front of the multi-store via WiFiCreds::add() so it lands at
    // slot 0 (highest priority) for the next attempt without evicting
    // working backups.
    scanAndPrintSSIDs();
    Serial.println("[WIFI] No saved network reachable. Send a fresh JSON to add one, or wait to stay offline.");
    if (!jarvis::NVSConfig::provisionWiFiFromSerial()) {
        Serial.println("[WIFI] No reprovisioning JSON received; staying offline.");
        return false;
    }
    String ssid = jarvis::NVSConfig::getWiFi0SSID();
    String pass = jarvis::NVSConfig::getWiFi0Pass();
    if (ssid.length()) jarvis::app::WiFiCreds::add(ssid, pass);
    return connectInSlotOrder(connectTimeoutMs);
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

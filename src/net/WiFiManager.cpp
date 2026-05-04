#include "WiFiManager.h"

#include <WiFi.h>
#include <WiFiMulti.h>

#include "../app/NVSConfig.h"

namespace jarvis::net {

static WiFiMulti g_wifi;

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

}  // namespace jarvis::net

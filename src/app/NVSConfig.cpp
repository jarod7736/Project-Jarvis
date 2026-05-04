#include "NVSConfig.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace jarvis {

static const char* NS = "jarvis";

String NVSConfig::getWiFi0SSID() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("wifi0_ssid", "");
    p.end();
    return s;
}

String NVSConfig::getWiFi0Pass() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("wifi0_pass", "");
    p.end();
    return s;
}

bool NVSConfig::setWiFi0(const String& ssid, const String& pass) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = (p.putString("wifi0_ssid", ssid) > 0) &&
              (p.putString("wifi0_pass", pass) > 0);
    p.end();
    return ok;
}

// Dump up to 200 bytes of `line`, escaping non-printable characters as <0xNN>
// so we can see exactly what arrived over USB Serial when JSON parsing fails.
// Critical for diagnosing shell-quoting / line-ending / encoding issues.
static void dumpRawLine(const String& line) {
    Serial.printf("[PROV] Raw input (%u bytes, first 200, non-printable escaped):\n",
                  (unsigned)line.length());
    Serial.print("[PROV] >>>");
    size_t lim = line.length() < 200 ? line.length() : 200;
    for (size_t i = 0; i < lim; ++i) {
        char c = line.charAt(i);
        if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
            Serial.print(c);
        } else {
            Serial.printf("<0x%02X>", (uint8_t)c);
        }
    }
    Serial.println("<<<");
}

bool NVSConfig::provisionWiFiFromSerial(uint32_t timeoutMs) {
    Serial.println();
    Serial.println("[PROV] Send JSON: {\"ssid\":\"<ssid>\",\"pass\":\"<password>\"}");
    Serial.printf ("[PROV] Listening on USB Serial for up to %lu seconds...\n",
                   (unsigned long)(timeoutMs / 1000));

    String line;
    line.reserve(256);
    uint32_t t0 = millis();

    while (millis() - t0 < timeoutMs) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (line.length() == 0) continue;

                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, line);
                if (err) {
                    Serial.printf("[PROV] JSON parse error: %s — try again.\n",
                                  err.c_str());
                    dumpRawLine(line);
                    line = "";
                    continue;
                }
                // Strict ssid validation: JsonVariant.as<String>() coerces JSON
                // null into the literal string "null" (length 4), which then
                // bypasses an emptiness check. Reject null variant, missing
                // key, non-string types, empty string, and the literal "null".
                JsonVariantConst ssidVar = doc["ssid"];
                if (ssidVar.isNull() || !ssidVar.is<const char*>()) {
                    Serial.println("[PROV] JSON 'ssid' is missing or null — try again.");
                    dumpRawLine(line);
                    line = "";
                    continue;
                }
                String ssid = ssidVar.as<String>();
                String pass = doc["pass"].as<String>();
                if (ssid.length() == 0 || ssid.equalsIgnoreCase("null")) {
                    Serial.println("[PROV] JSON 'ssid' is empty or literally \"null\" — try again.");
                    dumpRawLine(line);
                    line = "";
                    continue;
                }
                if (!setWiFi0(ssid, pass)) {
                    Serial.println("[PROV] Failed to write to NVS.");
                    return false;
                }
                Serial.printf("[PROV] Saved SSID=\"%s\" to NVS (password not echoed).\n",
                              ssid.c_str());
                return true;
            }
            line += c;
            if (line.length() > 512) {
                Serial.println("[PROV] Line too long, discarding buffer.");
                line = "";
            }
        }
        delay(20);
    }
    Serial.println("[PROV] Timeout waiting for provisioning JSON.");
    return false;
}

}  // namespace jarvis

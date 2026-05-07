#include "NVSConfig.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include "../config.h"

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

String NVSConfig::getHaToken() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("ha_token", "");
    p.end();
    return s;
}

bool NVSConfig::setHaToken(const String& token) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("ha_token", token) > 0;
    p.end();
    return ok;
}

String NVSConfig::getHaHost() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("ha_host", "");
    p.end();
    return s.length() ? s : String(jarvis::config::kHaHostDefault);
}

bool NVSConfig::setHaHost(const String& host) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("ha_host", host) > 0;
    p.end();
    return ok;
}

String NVSConfig::getOcKey() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("oc_key", "");
    p.end();
    return s;
}

bool NVSConfig::setOcKey(const String& key) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("oc_key", key) > 0;
    p.end();
    return ok;
}

String NVSConfig::getOcHost() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("oc_host", "");
    p.end();
    return s.length() ? s : String(jarvis::config::kOpenclawHostDefault);
}

bool NVSConfig::setOcHost(const String& host) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("oc_host", host) > 0;
    p.end();
    return ok;
}

String NVSConfig::getTtsProvider() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("tts_provider", "");
    p.end();
    return s.length() ? s : String(jarvis::config::kTtsProviderDefault);
}

bool NVSConfig::setTtsProvider(const String& provider) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("tts_provider", provider) > 0;
    p.end();
    return ok;
}

String NVSConfig::getTtsVoiceId() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("tts_voice_id", "");
    p.end();
    return s.length() ? s : String(jarvis::config::kTtsVoiceIdDefault);
}

bool NVSConfig::setTtsVoiceId(const String& voiceId) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("tts_voice_id", voiceId) > 0;
    p.end();
    return ok;
}

String NVSConfig::getTtsApiKey() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("tts_api_key", "");
    p.end();
    return s;
}

bool NVSConfig::setTtsApiKey(const String& key) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("tts_api_key", key) > 0;
    p.end();
    return ok;
}

String NVSConfig::getTtsModel() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("tts_model", "");
    p.end();
    return s.length() ? s : String(jarvis::config::kTtsModelDefault);
}

bool NVSConfig::setTtsModel(const String& model) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("tts_model", model) > 0;
    p.end();
    return ok;
}

String NVSConfig::getFwUrl() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("fw_url", "");
    p.end();
    return s;
}

bool NVSConfig::setFwUrl(const String& url) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("fw_url", url) > 0;
    p.end();
    return ok;
}

String NVSConfig::getOtaPass() {
    Preferences p;
    p.begin(NS, true);
    String s = p.getString("ota_pass", "");
    p.end();
    return s;
}

bool NVSConfig::setOtaPass(const String& pass) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString("ota_pass", pass) > 0;
    p.end();
    return ok;
}

// Apply a parsed JSON object to NVS. Each present key writes; absent keys
// are skipped. Returns true if at least one key was applied. Logs every
// applied key (without echoing secrets — token shows length only).
static bool applyProvisioningJson(const JsonDocument& doc) {
    bool any = false;

    JsonVariantConst ssid = doc["ssid"];
    JsonVariantConst pass = doc["pass"];
    if (ssid.is<const char*>() && pass.is<const char*>()) {
        String s = ssid.as<String>(), p = pass.as<String>();
        if (s.length() && !s.equalsIgnoreCase("null")) {
            if (NVSConfig::setWiFi0(s, p)) {
                Serial.printf("[PROV] Saved SSID=\"%s\" (pass not echoed)\n", s.c_str());
                any = true;
            } else {
                Serial.println("[PROV] Failed to write WiFi creds.");
            }
        }
    }

    JsonVariantConst tok = doc["ha_token"];
    if (tok.is<const char*>()) {
        String t = tok.as<String>();
        if (t.length() && !t.equalsIgnoreCase("null")) {
            if (NVSConfig::setHaToken(t)) {
                Serial.printf("[PROV] Saved ha_token (%u chars, value not echoed)\n",
                              (unsigned)t.length());
                any = true;
            } else {
                Serial.println("[PROV] Failed to write ha_token.");
            }
        }
    }

    JsonVariantConst host = doc["ha_host"];
    if (host.is<const char*>()) {
        String h = host.as<String>();
        if (h.length() && !h.equalsIgnoreCase("null")) {
            if (NVSConfig::setHaHost(h)) {
                Serial.printf("[PROV] Saved ha_host=\"%s\"\n", h.c_str());
                any = true;
            } else {
                Serial.println("[PROV] Failed to write ha_host.");
            }
        }
    }

    JsonVariantConst ock = doc["oc_key"];
    if (ock.is<const char*>()) {
        String k = ock.as<String>();
        if (k.length() && !k.equalsIgnoreCase("null")) {
            if (NVSConfig::setOcKey(k)) {
                Serial.printf("[PROV] Saved oc_key (%u chars, value not echoed)\n",
                              (unsigned)k.length());
                any = true;
            } else {
                Serial.println("[PROV] Failed to write oc_key.");
            }
        }
    }

    JsonVariantConst och = doc["oc_host"];
    if (och.is<const char*>()) {
        String h = och.as<String>();
        if (h.length() && !h.equalsIgnoreCase("null")) {
            if (NVSConfig::setOcHost(h)) {
                Serial.printf("[PROV] Saved oc_host=\"%s\"\n", h.c_str());
                any = true;
            } else {
                Serial.println("[PROV] Failed to write oc_host.");
            }
        }
    }

    // Phase 7 cloud TTS keys + OTA keys. Same null-handling pattern as the
    // others. Secrets (api_key, ota_pass) are echoed as length-only.
    struct StringField {
        const char* json;
        bool        secret;
        bool      (*setter)(const String&);
    };
    static const StringField string_fields[] = {
        {"tts_provider", false, &NVSConfig::setTtsProvider},
        {"tts_voice_id", false, &NVSConfig::setTtsVoiceId},
        {"tts_api_key",  true,  &NVSConfig::setTtsApiKey},
        {"tts_model",    false, &NVSConfig::setTtsModel},
        {"fw_url",       false, &NVSConfig::setFwUrl},
        {"ota_pass",     true,  &NVSConfig::setOtaPass},
    };
    for (const auto& f : string_fields) {
        JsonVariantConst v = doc[f.json];
        if (!v.is<const char*>()) continue;
        String s = v.as<String>();
        if (s.length() == 0 || s.equalsIgnoreCase("null")) continue;
        if (f.setter(s)) {
            if (f.secret) {
                Serial.printf("[PROV] Saved %s (%u chars, value not echoed)\n",
                              f.json, (unsigned)s.length());
            } else {
                Serial.printf("[PROV] Saved %s=\"%s\"\n", f.json, s.c_str());
            }
            any = true;
        } else {
            Serial.printf("[PROV] Failed to write %s.\n", f.json);
        }
    }

    return any;
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

// Read one JSON line from Serial. On success, fills `outDoc` and returns
// true. On parse error, logs and reverts to listening. On timeout, returns
// false. Shared body for the two provisioning entry points.
static bool readProvisioningJson(JsonDocument& outDoc, uint32_t timeoutMs) {
    String line;
    line.reserve(256);
    uint32_t t0 = millis();

    while (millis() - t0 < timeoutMs) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (line.length() == 0) continue;

                DeserializationError err = deserializeJson(outDoc, line);
                if (err) {
                    Serial.printf("[PROV] JSON parse error: %s — try again.\n", err.c_str());
                    dumpRawLine(line);
                    line = "";
                    continue;
                }
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

bool NVSConfig::provisionFromSerial(uint32_t timeoutMs) {
    Serial.println();
    Serial.println("[PROV] Send JSON with any of: ssid, pass, ha_token, ha_host");
    Serial.printf ("[PROV] Listening on USB Serial for up to %lu seconds...\n",
                   (unsigned long)(timeoutMs / 1000));

    JsonDocument doc;
    if (!readProvisioningJson(doc, timeoutMs)) return false;
    return applyProvisioningJson(doc);
}

bool NVSConfig::provisionWiFiFromSerial(uint32_t timeoutMs) {
    Serial.println();
    Serial.println("[PROV] Send JSON: {\"ssid\":\"<ssid>\",\"pass\":\"<password>\"}");
    Serial.printf ("[PROV] Listening on USB Serial for up to %lu seconds...\n",
                   (unsigned long)(timeoutMs / 1000));

    while (true) {
        JsonDocument doc;
        if (!readProvisioningJson(doc, timeoutMs)) return false;

        // Strict ssid validation: JsonVariant.as<String>() coerces JSON
        // null into the literal string "null" (length 4), which then
        // bypasses an emptiness check. Reject null variant, missing
        // key, non-string types, empty string, and the literal "null".
        JsonVariantConst ssidVar = doc["ssid"];
        if (ssidVar.isNull() || !ssidVar.is<const char*>()) {
            Serial.println("[PROV] JSON 'ssid' is missing or null — try again.");
            continue;
        }
        String ssid = ssidVar.as<String>();
        if (ssid.length() == 0 || ssid.equalsIgnoreCase("null")) {
            Serial.println("[PROV] JSON 'ssid' is empty or literally \"null\" — try again.");
            continue;
        }
        // Apply ALL provided keys (ssid+pass guaranteed; ha_token/ha_host
        // may be present too). Caller required ssid; the rest is bonus.
        return applyProvisioningJson(doc);
    }
}

}  // namespace jarvis

#include "OtaService.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

#include "../app/NVSConfig.h"
#include "../config.h"
#include "../hal/Display.h"

namespace jarvis::net {

namespace {

bool g_began  = false;
bool g_active = false;

// Map ArduinoOTA's error enum to a short tag for the serial log. The lib
// only hands us the int — translate so log greps actually find something.
const char* otaErrorName(ota_error_t err) {
    switch (err) {
        case OTA_AUTH_ERROR:    return "AUTH";
        case OTA_BEGIN_ERROR:   return "BEGIN";
        case OTA_CONNECT_ERROR: return "CONNECT";
        case OTA_RECEIVE_ERROR: return "RECEIVE";
        case OTA_END_ERROR:     return "END";
        default:                return "UNKNOWN";
    }
}

// Throttle progress logging. ArduinoOTA + HTTPUpdate fire onProgress every
// few KB; printing every callback floods the serial console and slows the
// flash. Print at most every 5%.
int g_last_pct_logged = -1;
void resetProgressLog() { g_last_pct_logged = -1; }

void logProgressIfStep(unsigned int cur, unsigned int total) {
    if (total == 0) return;
    int pct = (int)((uint64_t)cur * 100 / total);
    if (pct < g_last_pct_logged + 5 && pct < 100) return;
    g_last_pct_logged = pct;
    Serial.printf("[OTA] %d%% (%u / %u)\n", pct, cur, total);
}

}  // namespace

bool OtaService::begin() {
    String pass = jarvis::NVSConfig::getOtaPass();
    if (pass.length() == 0) {
        Serial.println("[OTA] disabled (no ota_pass in NVS)");
        return false;
    }

    ArduinoOTA.setHostname(jarvis::config::kOtaHostname);
    ArduinoOTA.setPort(jarvis::config::kOtaPort);
    ArduinoOTA.setPassword(pass.c_str());

    ArduinoOTA.onStart([]() {
        const char* type = (ArduinoOTA.getCommand() == U_FLASH) ? "flash" : "fs";
        Serial.printf("[OTA] start type=%s\n", type);
        g_active = true;
        resetProgressLog();
        jarvis::hal::Display::setOtaActive(true);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] end (rebooting)");
        g_active = false;
        jarvis::hal::Display::setOtaActive(false);
    });

    ArduinoOTA.onError([](ota_error_t err) {
        Serial.printf("[OTA] error=%s (%u)\n", otaErrorName(err), (unsigned)err);
        g_active = false;
        jarvis::hal::Display::setOtaActive(false);
    });

    ArduinoOTA.onProgress([](unsigned int cur, unsigned int total) {
        // Feed the loop watchdog — ArduinoOTA's internal recv loop
        // doesn't return to loop() during a flash, so loop()'s
        // esp_task_wdt_reset() can't fire. Without this the 30 s
        // timeout panics the device mid-flash.
        esp_task_wdt_reset();
        logProgressIfStep(cur, total);
    });

    ArduinoOTA.begin();
    g_began = true;
    Serial.printf("[OTA] ready hostname=%s.local port=%u\n",
                  jarvis::config::kOtaHostname,
                  (unsigned)jarvis::config::kOtaPort);
    return true;
}

void OtaService::tick() {
    if (!g_began) return;
    ArduinoOTA.handle();
}

bool OtaService::isActive() {
    return g_active;
}

bool OtaService::pullRemote(const String& url) {
    if (url.length() == 0) {
        Serial.println("[OTA] pullRemote: empty URL");
        return false;
    }
    bool is_https = url.startsWith("https://");
    if (!is_https && !url.startsWith("http://")) {
        Serial.printf("[OTA] pullRemote: unsupported scheme in url=\"%s\"\n", url.c_str());
        return false;
    }

    Serial.printf("[OTA] pullRemote: %s\n", url.c_str());
    g_active = true;
    resetProgressLog();
    jarvis::hal::Display::setOtaActive(true);

    // CoreS3 has no built-in OTA-progress LED. -1 disables the lib's
    // attempt to drive a pin we don't have wired.
    httpUpdate.setLedPin(-1);
    httpUpdate.rebootOnUpdate(true);

    httpUpdate.onProgress([](int cur, int total) {
        esp_task_wdt_reset();
        logProgressIfStep((unsigned)cur, (unsigned)total);
    });

    t_httpUpdate_return result;
    if (is_https) {
        // setInsecure: skip cert validation. Cert pinning is a deferred
        // TODO per CLAUDE.md, same posture as HAClient and LLMClient.
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(jarvis::config::kOtaHttpTimeoutMs / 1000);
        result = httpUpdate.update(client, url);
    } else {
        WiFiClient client;
        client.setTimeout(jarvis::config::kOtaHttpTimeoutMs / 1000);
        result = httpUpdate.update(client, url);
    }

    // If we got here the device did NOT reboot — either the update
    // failed or the server replied "no update". Either way clear active.
    g_active = false;
    jarvis::hal::Display::setOtaActive(false);

    switch (result) {
        case HTTP_UPDATE_FAILED:
            Serial.printf("[OTA] FAILED: code=%d msg=\"%s\"\n",
                          httpUpdate.getLastError(),
                          httpUpdate.getLastErrorString().c_str());
            return false;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("[OTA] server replied: no update available");
            return false;
        case HTTP_UPDATE_OK:
            // Should be unreachable — rebootOnUpdate(true) means we
            // restarted before returning. Treat as success defensively.
            Serial.println("[OTA] OK (no reboot?)");
            return true;
    }
    return false;
}

}  // namespace jarvis::net

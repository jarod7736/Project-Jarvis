#include "HAClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "../app/NVSConfig.h"

namespace jarvis::net {

namespace {

// Build the base URL once per call. Nabu Casa requires HTTPS; local HA can
// also speak HTTPS via its proxy. We assume HTTPS unconditionally — if a user
// has plain-HTTP local HA they can set ha_host to "http://192.168.x.x:8123"
// and that still works because HTTPClient parses the scheme.
String makeUrl(const String& host, const char* path) {
    String url;
    if (host.startsWith("http://") || host.startsWith("https://")) {
        url = host;
    } else {
        url = "https://" + host;
    }
    url += path;
    return url;
}

// One blocking HTTP call. Always calls http.end() before returning so the
// WiFiClientSecure + HTTPClient leak CLAUDE.md flags can't bite us.
//
// Method: "GET" or "POST". Body is the request body (empty for GET).
// On success returns >0 HTTP status; on transport failure returns negative
// HTTPClient error code. `outBody` filled with response body on 2xx.
int doRequest(const char* method, const String& url, const String& token,
              const String& body, String* outBody) {
    WiFiClientSecure client;
    client.setInsecure();    // CLAUDE.md: cert pinning is a deferred TODO

    HTTPClient http;
    http.setTimeout(8000);
    http.useHTTP10(true);    // CLAUDE.md: avoid chunked transfer encoding

    if (!http.begin(client, url)) {
        return -100;
    }
    http.addHeader("Authorization", String("Bearer ") + token);
    http.addHeader("Content-Type",  "application/json");

    int code;
    if (strcmp(method, "POST") == 0) {
        code = http.POST(body);
    } else {
        code = http.GET();
    }

    if (code >= 200 && code < 300 && outBody) {
        *outBody = http.getString();
    }

    http.end();
    return code;
}

}  // namespace

bool HAClient::isConfigured() {
    return jarvis::NVSConfig::getHaToken().length() > 0 &&
           jarvis::NVSConfig::getHaHost().length()  > 0;
}

bool HAClient::callService(const char* domain, const char* service,
                           const char* entityId) {
    if (!isConfigured()) {
        Serial.println("[HA] callService: not configured (no token/host)");
        return false;
    }

    String host  = jarvis::NVSConfig::getHaHost();
    String token = jarvis::NVSConfig::getHaToken();

    String path = "/api/services/" + String(domain) + "/" + String(service);
    String url  = makeUrl(host, path.c_str());

    char body[256];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entityId);

    Serial.printf("[HA] POST %s body=%s\n", url.c_str(), body);
    int code = doRequest("POST", url, token, String(body), nullptr);
    Serial.printf("[HA] -> %d\n", code);
    return code >= 200 && code < 300;
}

String HAClient::getState(const char* entityId) {
    if (!isConfigured()) {
        Serial.println("[HA] getState: not configured (no token/host)");
        return String();
    }

    String host  = jarvis::NVSConfig::getHaHost();
    String token = jarvis::NVSConfig::getHaToken();

    String path = "/api/states/" + String(entityId);
    String url  = makeUrl(host, path.c_str());

    Serial.printf("[HA] GET %s\n", url.c_str());
    String body;
    int code = doRequest("GET", url, token, String(), &body);
    if (code < 200 || code >= 300) {
        Serial.printf("[HA] -> %d (state query failed)\n", code);
        return String();
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[HA] state JSON parse failed: %s\n", err.c_str());
        return String();
    }
    String state = doc["state"].as<String>();
    Serial.printf("[HA] -> %d state=%s\n", code, state.c_str());
    return state;
}

}  // namespace jarvis::net

#include "CaptivePortal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "../app/ConfigSchema.h"
#include "../app/WiFiCreds.h"

namespace jarvis::net {

namespace {
AsyncWebServer g_server(80);
DNSServer      g_dns;
bool           g_running = false;
bool           g_exit_requested = false;

// AsyncWebServer fragments POST bodies; we accumulate manually per route.
struct PendingBody { String data; };
PendingBody g_pending_config;
PendingBody g_pending_wifi;

void handleGetConfig(AsyncWebServerRequest* req) {
    JsonDocument doc;
    jarvis::app::buildConfigJson(doc);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void handlePostConfigBody(AsyncWebServerRequest* req,
                          uint8_t* data, size_t len, size_t idx, size_t total) {
    if (idx == 0) g_pending_config.data = "";
    g_pending_config.data.concat(reinterpret_cast<const char*>(data), len);
    if (idx + len < total) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, g_pending_config.data);
    g_pending_config.data = "";
    if (err) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    int n = jarvis::app::applyConfigJson(doc);
    if (n < 0) {
        req->send(400, "application/json", "{\"error\":\"validation failed\"}");
        return;
    }
    JsonDocument resp;
    resp["updated"] = n;
    String out; serializeJson(resp, out);
    req->send(200, "application/json", out);
}

void handleWifiScan(AsyncWebServerRequest* req) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        req->send(202, "application/json", "{\"status\":\"scanning\"}");
        return;
    }
    if (n < 0) {
        WiFi.scanNetworks(/*async=*/true);
        req->send(202, "application/json", "{\"status\":\"started\"}");
        return;
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]   = WiFi.SSID(i);
        o["rssi"]   = WiFi.RSSI(i);
        o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    String out; serializeJson(doc, out);
    WiFi.scanDelete();
    req->send(200, "application/json", out);
}

void handleWifiSaved(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    jarvis::app::WiFiCreds::list(arr);
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void handlePostWifiAddBody(AsyncWebServerRequest* req,
                           uint8_t* data, size_t len, size_t idx, size_t total) {
    if (idx == 0) g_pending_wifi.data = "";
    g_pending_wifi.data.concat(reinterpret_cast<const char*>(data), len);
    if (idx + len < total) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, g_pending_wifi.data);
    g_pending_wifi.data = "";
    if (err) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    String ssid = doc["ssid"].as<String>();
    String pass = doc["password"].as<String>();
    if (ssid.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
    }
    jarvis::app::WiFiCreds::add(ssid, pass);
    req->send(200, "application/json", "{\"saved\":true}");
}

void handleWifiRemove(AsyncWebServerRequest* req) {
    if (!req->hasParam("ssid")) {
        req->send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
    }
    jarvis::app::WiFiCreds::remove(req->getParam("ssid")->value());
    req->send(200, "application/json", "{\"removed\":true}");
}

void handleStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["battery_pct"] = M5.Power.getBatteryLevel();
    doc["charging"]    = M5.Power.isCharging();
    doc["uptime_ms"]   = millis();
    doc["free_heap"]   = ESP.getFreeHeap();
    doc["mode"]        = "config";
    doc["ap_ssid"]     = "Jarvis-Setup";
    doc["ap_clients"]  = WiFi.softAPgetStationNum();
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void handleExit(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"exiting\":true}");
    g_exit_requested = true;
}

void handleNotFound(AsyncWebServerRequest* req) {
    // Captive-portal redirect target. iOS hits /hotspot-detect.html,
    // Android /generate_204, Windows /connecttest.txt — sending them all
    // to the AP root triggers the OS-level "sign in to network" UI.
    req->redirect("http://192.168.4.1/");
}

void registerRoutes() {
    // Static UI from LittleFS.
    g_server.serveStatic("/", LittleFS, "/web/")
            .setDefaultFile("index.html");

    g_server.on("/api/config", HTTP_GET, handleGetConfig);
    g_server.on("/api/config", HTTP_POST,
                [](AsyncWebServerRequest* req) {},
                nullptr,
                handlePostConfigBody);

    g_server.on("/api/wifi/scan",  HTTP_GET,  handleWifiScan);
    g_server.on("/api/wifi/saved", HTTP_GET,  handleWifiSaved);
    g_server.on("/api/wifi/add",   HTTP_POST,
                [](AsyncWebServerRequest* req) {},
                nullptr,
                handlePostWifiAddBody);
    g_server.on("/api/wifi/remove", HTTP_POST, handleWifiRemove);
    g_server.on("/api/status",     HTTP_GET,  handleStatus);
    g_server.on("/api/exit",       HTTP_POST, handleExit);

    g_server.onNotFound(handleNotFound);
}
}  // namespace

void CaptivePortal::begin() {
    if (g_running) return;
    g_exit_requested = false;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Jarvis-Setup", nullptr);

    registerRoutes();
    g_server.begin();
    g_dns.start(53, "*", IPAddress(192, 168, 4, 1));
    g_running = true;

    Serial.print("[Portal] AP up — ");
    Serial.println(WiFi.softAPIP());
}

void CaptivePortal::end() {
    if (!g_running) return;
    g_dns.stop();
    g_server.end();
    g_running = false;
    Serial.println("[Portal] stopped");
}

void CaptivePortal::tick() {
    if (!g_running) return;
    g_dns.processNextRequest();
}

bool CaptivePortal::running()       { return g_running; }
bool CaptivePortal::exitRequested() { return g_exit_requested; }
void CaptivePortal::clearExitFlag() { g_exit_requested = false; }

}  // namespace jarvis::net

#include "MqttClient.h"

#include <PubSubClient.h>
#include <WiFi.h>

#include "../app/NVSConfig.h"
#include "../config.h"

namespace jarvis::net {

namespace {

// Single global state — same single-threaded model as the rest of the
// codebase, so no atomics needed.
WiFiClient   g_wifi;
PubSubClient g_mqtt(g_wifi);

bool     g_began           = false;
String   g_host;
String   g_user;
String   g_pass;
String   g_client_id;
uint32_t g_last_attempt_ms = 0;

// Single-slot command queue. The PubSubClient callback fills this; the
// FSM drains via popPendingCommand(). A second arrival before the first
// is consumed overwrites — matches the KWS-during-active behavior in
// state_machine.cpp ("noise during a session is dropped").
String g_pending_command;
bool   g_pending_command_set = false;

// Build a stable-per-device client ID. PubSubClient requires unique IDs
// across the broker; "jarvis" alone would collide if a future deployment
// has multiple devices. Suffix with the last 6 hex chars of the WiFi MAC.
String buildClientId() {
    String mac = WiFi.macAddress();          // "F4:12:FA:BA:0E:04"
    mac.replace(":", "");                    // "F412FABA0E04"
    String tail = mac.length() >= 6 ? mac.substring(mac.length() - 6) : mac;
    String id = String(jarvis::config::kMqttClientIdBase) + "-" + tail;
    return id;
}

void onMessage(char* topic, byte* payload, unsigned int len) {
    // Topic dispatch. Today there's only the command topic; keep the
    // structure so adding more later is straightforward.
    if (strcmp(topic, jarvis::config::kMqttTopicCommand) == 0) {
        // Copy out — PubSubClient reuses the payload buffer for the next
        // message. Cap to 256 chars for sanity (transcripts are short).
        size_t take = len > 256 ? 256 : len;
        String s;
        s.reserve(take);
        for (size_t i = 0; i < take; ++i) s += (char)payload[i];
        g_pending_command     = s;
        g_pending_command_set = true;
        Serial.printf("[MQTT] command received (%u bytes): \"%s\"\n",
                      (unsigned)take, s.c_str());
    } else {
        Serial.printf("[MQTT] unhandled topic: %s (%u bytes)\n",
                      topic, len);
    }
}

// Attempt one (re)connect to the broker. Non-blocking in spirit — the
// underlying TCP connect can stall up to PubSubClient's socket timeout
// (~15s on failure). Called from tick() with kMqttReconnectMs gating, so
// we only burn that hang at most once every 30 s on a dead broker.
bool tryConnect() {
    if (!WiFi.isConnected()) return false;

    Serial.printf("[MQTT] connecting to %s:%u as %s (id=%s)\n",
                  g_host.c_str(), (unsigned)jarvis::config::kMqttPort,
                  g_user.length() ? g_user.c_str() : "<anon>",
                  g_client_id.c_str());

    // connect(client_id, user, pass, willTopic, willQos, willRetain,
    //         willMessage, cleanSession). Last-Will publishes "OFFLINE"
    // to jarvis/state if the broker drops us without a clean disconnect
    // — lets HA detect a hung Jarvis without polling.
    bool ok;
    if (g_user.length() > 0) {
        ok = g_mqtt.connect(
            g_client_id.c_str(),
            g_user.c_str(), g_pass.c_str(),
            jarvis::config::kMqttTopicState,
            /*willQos=*/0, /*willRetain=*/true,
            jarvis::config::kMqttLwtPayload,
            /*cleanSession=*/true);
    } else {
        // Anonymous broker variant — same LWT semantics.
        ok = g_mqtt.connect(
            g_client_id.c_str(),
            nullptr, nullptr,
            jarvis::config::kMqttTopicState,
            0, true,
            jarvis::config::kMqttLwtPayload,
            true);
    }

    if (!ok) {
        Serial.printf("[MQTT] connect failed: state=%d\n", g_mqtt.state());
        return false;
    }

    Serial.println("[MQTT] connected");
    g_mqtt.subscribe(jarvis::config::kMqttTopicCommand);
    Serial.printf("[MQTT] subscribed: %s\n", jarvis::config::kMqttTopicCommand);
    return true;
}

}  // namespace

bool MqttClient::begin() {
    g_host = jarvis::NVSConfig::getMqttHost();
    if (g_host.length() == 0) {
        Serial.println("[MQTT] disabled (no mqtt_host in NVS)");
        return false;
    }
    g_user      = jarvis::NVSConfig::getMqttUser();
    g_pass      = jarvis::NVSConfig::getMqttPass();
    g_client_id = buildClientId();

    g_mqtt.setServer(g_host.c_str(), jarvis::config::kMqttPort);
    g_mqtt.setCallback(onMessage);
    g_mqtt.setKeepAlive(jarvis::config::kMqttKeepaliveSec);
    // Bigger buffer than the 256-byte default — voice transcripts stay
    // short but state JSON could grow if we publish more later.
    g_mqtt.setBufferSize(512);

    g_began = true;
    Serial.printf("[MQTT] ready host=%s port=%u user=%s\n",
                  g_host.c_str(), (unsigned)jarvis::config::kMqttPort,
                  g_user.length() ? g_user.c_str() : "<anon>");

    // Don't block setup() on the first connect — let tick() handle it.
    g_last_attempt_ms = millis() - jarvis::config::kMqttReconnectMs;
    return true;
}

void MqttClient::tick() {
    if (!g_began) return;

    if (g_mqtt.connected()) {
        g_mqtt.loop();
        return;
    }

    // Disconnected — attempt a reconnect at most once per
    // kMqttReconnectMs to avoid hammering the broker during outages.
    uint32_t now = millis();
    if (now - g_last_attempt_ms < jarvis::config::kMqttReconnectMs) {
        return;
    }
    g_last_attempt_ms = now;
    tryConnect();
}

bool MqttClient::publish(const char* topic, const char* payload, bool retained) {
    if (!g_began || !g_mqtt.connected()) return false;
    return g_mqtt.publish(topic, payload, retained);
}

bool MqttClient::publishState(const char* state_name) {
    return publish(jarvis::config::kMqttTopicState, state_name, /*retained=*/true);
}

bool MqttClient::isConnected() {
    return g_began && g_mqtt.connected();
}

String MqttClient::popPendingCommand() {
    if (!g_pending_command_set) return String();
    String out = g_pending_command;
    g_pending_command     = "";
    g_pending_command_set = false;
    return out;
}

}  // namespace jarvis::net

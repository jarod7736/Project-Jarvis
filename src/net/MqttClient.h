#pragma once

// MqttClient — Phase 7 MQTT integration.
//
// Publishes the FSM state on every transition to `jarvis/state` (retained
// so any new HA subscriber sees the current state immediately). Subscribes
// to `jarvis/command` so HA automations can push text commands to Jarvis,
// which the FSM treats as if they were ASR transcripts.
//
// Per CLAUDE.md invariants:
//  - The PubSubClient incoming-message callback only buffers the payload —
//    state transitions happen in tickStateMachine() like every other input.
//  - Reconnect is non-blocking: when down, we attempt to (re)connect once
//    every kMqttReconnectMs and bail out otherwise.
//  - When `mqtt_host` is empty in NVS, the entire client stays inert.

#include <Arduino.h>

namespace jarvis::net {

class MqttClient {
public:
    // Read mqtt_* keys from NVS, set up PubSubClient, attempt initial
    // connect. Safe to call before WiFi is up — first connect will fail
    // and the reconnect timer takes over. Returns true if the broker
    // host is configured (independent of whether the connect succeeded).
    static bool begin();

    // Drive PubSubClient.loop() and the reconnect cadence. Cheap when
    // disabled (mqtt_host empty). Call every loop iteration.
    static void tick();

    // Publish to an arbitrary topic. Returns true iff the message was
    // queued by PubSubClient. Retained payloads stick on the broker.
    static bool publish(const char* topic, const char* payload, bool retained = false);

    // Convenience: publish FSM state (retained). state_name should be
    // one of "IDLE" / "LISTENING" / "THINKING" / "SPEAKING" / "ERROR".
    static bool publishState(const char* state_name);

    // True when broker connection is live.
    static bool isConnected();

    // Pop a pending command payload, if one arrived since the last call.
    // Returns empty String when the queue is empty. Single-slot — a new
    // arrival overwrites an unconsumed one (commands during SPEAKING
    // are dropped, mirroring KWS-during-active behavior in state_machine).
    static String popPendingCommand();
};

}  // namespace jarvis::net

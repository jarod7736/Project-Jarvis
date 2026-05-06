#pragma once

// HAClient — Phase 4 Home Assistant REST client.
//
// Backend: Nabu Casa cloud HTTPS (or local IP — both work, host comes from
// NVS via NVSConfig::getHaHost). Bearer auth from NVS `ha_token`.
//
// All calls are blocking (CLAUDE.md HTTP rule). Caller must show "Thinking..."
// on the display BEFORE calling — loop() stalls during the network round-trip.
//
// Every exit path guarantees http.end() to avoid the WiFiClientSecure +
// HTTPClient leak that CLAUDE.md flags.

#include <Arduino.h>

namespace jarvis::net {

class HAClient {
public:
    // True iff NVS has both ha_token and ha_host (or default host applies).
    // Cheap check, doesn't probe the server. Use to decide whether to dispatch
    // an HA intent at all.
    static bool isConfigured();

    // POST /api/services/<domain>/<service> with JSON body {"entity_id": ...}.
    // Returns true on HTTP 2xx. Common failures: token invalid (401),
    // entity unknown (404), HA down (timeout/connect-fail).
    static bool callService(const char* domain, const char* service,
                            const char* entityId);

    // GET /api/states/<entity_id>. Returns the entity's `state` field
    // (e.g. "on", "off", "open", "closed", "22.5") on success, empty
    // String on any failure.
    static String getState(const char* entityId);
};

}  // namespace jarvis::net

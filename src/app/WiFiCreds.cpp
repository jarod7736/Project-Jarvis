#include "WiFiCreds.h"

#include <Preferences.h>

#include "NVSConfig.h"

namespace jarvis::app {

namespace {
constexpr const char* NS = "wifi";

// Slot keys are "s<i>" / "p<i>" — 2 chars + a digit, well under the 15-
// char NVS namespace limit. The count is at "n" (a uchar 0..kMaxNetworks).
void slotKeys(size_t i, char* skey, char* pkey, size_t bufLen) {
    snprintf(skey, bufLen, "s%zu", i);
    snprintf(pkey, bufLen, "p%zu", i);
}

void writeAll(WiFiCreds::Network* nets, size_t count) {
    Preferences p;
    p.begin(NS, false);
    p.putUChar("n", static_cast<uint8_t>(count));
    char sk[8], pk[8];
    for (size_t i = 0; i < count; ++i) {
        slotKeys(i, sk, pk, sizeof(sk));
        p.putString(sk, nets[i].ssid);
        p.putString(pk, nets[i].password);
    }
    p.end();
}

// Lazy one-time migration: if the multi-slot store is empty but the
// legacy NVSConfig slot has an SSID, copy it over. Idempotent.
void migrateLegacyIfNeeded() {
    Preferences p;
    p.begin(NS, true);
    uint8_t n = p.getUChar("n", 0);
    p.end();
    if (n > 0) return;

    String legacy = jarvis::NVSConfig::getWiFi0SSID();
    if (legacy.length() == 0) return;
    String legacyPass = jarvis::NVSConfig::getWiFi0Pass();

    WiFiCreds::Network nets[1] = {{legacy, legacyPass}};
    writeAll(nets, 1);
    Serial.printf("[WiFiCreds] migrated legacy SSID=\"%s\" into \"wifi\" namespace\n",
                  legacy.c_str());
}
}  // namespace

void WiFiCreds::load(Network out[kMaxNetworks], size_t& count) {
    migrateLegacyIfNeeded();

    Preferences p;
    p.begin(NS, true);
    count = p.getUChar("n", 0);
    if (count > kMaxNetworks) count = kMaxNetworks;
    char sk[8], pk[8];
    for (size_t i = 0; i < count; ++i) {
        slotKeys(i, sk, pk, sizeof(sk));
        out[i].ssid     = p.getString(sk, "");
        out[i].password = p.getString(pk, "");
    }
    p.end();
}

bool WiFiCreds::add(const String& ssid, const String& password) {
    if (ssid.length() == 0) return false;

    Network nets[kMaxNetworks];
    size_t count = 0;
    load(nets, count);

    // If already present, update password and move to front.
    int existing = -1;
    for (size_t i = 0; i < count; ++i) {
        if (nets[i].ssid == ssid) { existing = static_cast<int>(i); break; }
    }
    if (existing >= 0) {
        Network updated = {ssid, password};
        // Slide entries [0..existing) down by one; place updated at [0].
        for (int i = existing; i > 0; --i) nets[i] = nets[i - 1];
        nets[0] = updated;
    } else {
        size_t newCount = count < kMaxNetworks ? count + 1 : kMaxNetworks;
        // Slide [0..newCount-1) right by one (drops oldest if full).
        for (size_t i = newCount - 1; i > 0; --i) nets[i] = nets[i - 1];
        nets[0] = {ssid, password};
        count = newCount;
    }
    writeAll(nets, count);
    return true;
}

bool WiFiCreds::remove(const String& ssid) {
    Network nets[kMaxNetworks];
    size_t count = 0;
    load(nets, count);

    size_t out = 0;
    for (size_t i = 0; i < count; ++i) {
        if (nets[i].ssid != ssid) {
            if (out != i) nets[out] = nets[i];
            ++out;
        }
    }
    if (out == count) return false;  // not present
    writeAll(nets, out);
    return true;
}

void WiFiCreds::list(JsonArray arr) {
    Network nets[kMaxNetworks];
    size_t count = 0;
    load(nets, count);
    for (size_t i = 0; i < count; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]     = nets[i].ssid;
        o["priority"] = static_cast<int>(i);
    }
}

}  // namespace jarvis::app

#include "CommandHandler.h"

#include "../config.h"
#include "../net/HAClient.h"

namespace jarvis::app {

namespace {

// PLAN.md spec table. The keyword strings are matched against lowercased
// transcript via String::indexOf — order matters, more specific first.
// Entity IDs reference HA defaults the user may need to adjust in their HA
// install.
struct Entry {
    const char* keyword;     // substring to look for in lowercased transcript
    const char* domain;      // HA service domain, or nullptr for state queries
    const char* service;     // HA service, or nullptr
    const char* entityId;    // HA entity_id
    const char* spokenName;  // human-readable name for TTS confirmation/query
};

constexpr Entry kCommands[] = {
    // More specific phrasings before less specific (e.g. "turn off" before "turn on"
    // before "lights"). String::indexOf is case-sensitive so we lowercase first.
    {"turn off bedroom",       "light",     "turn_off",     "light.bedroom",            "bedroom lights"},
    {"turn on bedroom",        "light",     "turn_on",      "light.bedroom",            "bedroom lights"},
    {"turn off the office",    "light",     "turn_off",     "light.office_1_light",     "office light"},
    {"turn on the office",     "light",     "turn_on",      "light.office_1_light",     "office light"},
    {"turn off office",        "light",     "turn_off",     "light.office_1_light",     "office light"},
    {"turn on office",         "light",     "turn_on",      "light.office_1_light",     "office light"},
    {"is the office on",        nullptr,    nullptr,        "light.office_1_light",     "office light"},
    {"turn off the light",     "light",     "turn_off",     "light.living_room",        "living room lights"},
    {"turn on the light",      "light",     "turn_on",      "light.living_room",        "living room lights"},
    {"turn off lights",        "light",     "turn_off",     "light.living_room",        "living room lights"},
    {"turn on lights",         "light",     "turn_on",      "light.living_room",        "living room lights"},
    {"lock the door",          "lock",      "lock",         "lock.front_door",          "front door"},
    {"unlock the door",        "lock",      "unlock",       "lock.front_door",          "front door"},
    {"close the garage",       "cover",     "close_cover",  "cover.garage_door",        "garage door"},
    {"is the garage open",      nullptr,    nullptr,        "cover.garage_door",        "garage door"},
    {"temperature",             nullptr,    nullptr,        "sensor.living_room_temp",  "temperature"},
    {"how warm",                nullptr,    nullptr,        "sensor.living_room_temp",  "temperature"},
    {"turn on fan",            "fan",       "turn_on",      "fan.bedroom",              "fan"},
    {"turn off fan",           "fan",       "turn_off",     "fan.bedroom",              "fan"},
};

constexpr const char* kConfirmations[] = { "Done", "Got it", "Okay" };

const char* pickConfirmation() {
    // Cheap rotation — not random, but unpredictable enough across multiple
    // commands in a session that it doesn't sound robotic.
    static uint8_t i = 0;
    const char* s = kConfirmations[i % (sizeof(kConfirmations)/sizeof(*kConfirmations))];
    ++i;
    return s;
}

}  // namespace

CommandResult dispatch(const String& transcript) {
    CommandResult r{false, false, String()};

    String t = transcript;
    t.toLowerCase();
    t.trim();
    if (t.length() == 0) return r;

    for (const Entry& e : kCommands) {
        if (t.indexOf(e.keyword) < 0) continue;

        r.handled = true;

        if (e.domain == nullptr) {
            // State query
            String state = jarvis::net::HAClient::getState(e.entityId);
            if (state.length() == 0) {
                r.ok     = false;
                r.spoken = jarvis::config::kErrHaUnreachable;
                return r;
            }
            r.ok     = true;
            r.spoken = "The " + String(e.spokenName) + " is " + state + ".";
            return r;
        }

        // Service call
        bool ok = jarvis::net::HAClient::callService(e.domain, e.service, e.entityId);
        r.ok     = ok;
        r.spoken = ok ? String(pickConfirmation()) + "."
                      : String(jarvis::config::kErrHaUnreachable);
        return r;
    }

    return r;  // handled=false
}

}  // namespace jarvis::app

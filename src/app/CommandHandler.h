#pragma once

// CommandHandler — Phase 4 hardcoded HA command table.
//
// PLAN.md is explicit that this is intentionally brittle: linear-scan via
// String::indexOf with a static list of 10 commands. It validates HAClient
// independently from intent classification. Phase 5 replaces dispatch() with
// the Qwen IntentRouter.
//
// Adding a new command in this phase = appending an entry to the table in
// CommandHandler.cpp. Don't add new intent kinds; that's Phase 5's job.

#include <Arduino.h>

namespace jarvis::app {

struct CommandResult {
    bool   handled;     // false = no entry matched
    bool   ok;          // true if HA call succeeded (always false when !handled)
    String spoken;      // text to feed into TTS — confirmation, state, or error
};

// Match `transcript` against the static command table. On match:
//   - For commands: posts to HA, returns confirmation phrase ("Done", etc.).
//   - For state queries: GETs the entity, returns "The garage door is open".
// On no match: returns {handled=false, ok=false, spoken=""} so the caller
// can fall through to the LLM path (Phase 5+) — for Phase 4, the caller
// just speaks the error string from config::kErrIntentParse.
CommandResult dispatch(const String& transcript);

}  // namespace jarvis::app

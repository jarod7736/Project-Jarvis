#pragma once

// IntentRouter — Phase 5 Qwen-based intent classifier and dispatcher.
//
// Replaces CommandHandler's keyword table as the primary entry point from
// the FSM. CommandHandler is still used internally for ha_command/ha_query
// entity resolution (Qwen's entity guesses are unreliable; the table maps
// from natural-language phrasing to actual HA entity_ids in the user's
// install).
//
// Flow:
//   transcript -> queryLlm(intent_prompt + transcript) -> JSON
//   parse JSON -> dispatch on `intent` field
//   on parse failure: fall back to CommandHandler keyword path
//
// All blocking; FSM must already be in THINKING display state before
// calling per CLAUDE.md's HTTP/UART rule.

#include <Arduino.h>

namespace jarvis::hal { class LLMModule; }
namespace jarvis::net { enum class ConnectivityTier; }

namespace jarvis::app {

struct RouteResult {
    bool   ok;        // true = dispatch succeeded (HA call, on_device, etc.)
    String spoken;    // text to feed TTS
};

// Bind the LLMModule once at boot. Without this, route() falls back
// directly to CommandHandler.
void intentRouterBegin(jarvis::hal::LLMModule* module);

// Route a user transcript. Tier informs whether to attempt local-LLM /
// claude (which need network) or stay on-device. Always returns a
// non-empty spoken string — never returns "" so the FSM has something
// to show + speak.
RouteResult route(const String& transcript, jarvis::net::ConnectivityTier tier);

}  // namespace jarvis::app

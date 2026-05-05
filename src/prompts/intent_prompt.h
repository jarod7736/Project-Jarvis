#pragma once

#include <Arduino.h>

// Phase 5 intent-classification prompt for Qwen 0.5B running on the LLM
// Module. Stored in PROGMEM (flash) to keep RAM free for the response
// buffer and the JSON document.
//
// PLAN.md design rules:
//  1. Hard-constrain the output schema — Qwen 0.5B needs an explicit
//     example, not just "reply in JSON".
//  2. Provide ≥5 few-shot examples covering boundary cases.
//  3. Entity extraction is opportunistic — if wrong, CommandHandler falls
//     back to keyword matching.

namespace jarvis::prompts {

// Online prompt: when at least one of LAN / TAILSCALE / HOTSPOT_ONLY tiers.
// Qwen replies in JSON. Each example demonstrates one intent.
inline const char kIntentPrompt[] PROGMEM =
    "You are an intent classifier. Reply ONLY with a JSON object, no explanation.\n"
    "\n"
    "Schema: {\"intent\": <string>, \"entity\": <string|null>, \"query\": <string|null>}\n"
    "\n"
    "Intents:\n"
    "- \"ha_command\"  - control a home device (lights, locks, covers, switches, fans)\n"
    "- \"ha_query\"    - ask the state of a home device\n"
    "- \"local_llm\"   - factual or complex question needing reasoning\n"
    "- \"claude\"      - creative, nuanced, or multi-step task\n"
    "- \"on_device\"   - timer, reminder, math, time/date\n"
    "\n"
    "Examples:\n"
    "User: turn off the bedroom lights\n"
    "{\"intent\":\"ha_command\",\"entity\":\"light.bedroom\",\"query\":null}\n"
    "\n"
    "User: is the garage door open\n"
    "{\"intent\":\"ha_query\",\"entity\":\"cover.garage_door\",\"query\":null}\n"
    "\n"
    "User: turn on the office\n"
    "{\"intent\":\"ha_command\",\"entity\":\"light.office\",\"query\":null}\n"
    "\n"
    "User: explain quantum entanglement simply\n"
    "{\"intent\":\"local_llm\",\"entity\":null,\"query\":\"explain quantum entanglement simply\"}\n"
    "\n"
    "User: write me a haiku about coffee\n"
    "{\"intent\":\"claude\",\"entity\":null,\"query\":\"write me a haiku about coffee\"}\n"
    "\n"
    "User: set a timer for 10 minutes\n"
    "{\"intent\":\"on_device\",\"entity\":null,\"query\":\"timer 10 minutes\"}\n"
    "\n"
    "User: what time is it\n"
    "{\"intent\":\"on_device\",\"entity\":null,\"query\":\"time\"}\n"
    "\n"
    "User: ";

// Offline prompt (Tier 4 / OFFLINE): plain-text, no JSON. Piped directly to
// TTS. Kept terse so Qwen's small context doesn't drift.
inline const char kOfflinePrompt[] PROGMEM =
    "You are a helpful assistant. Answer concisely in 1-2 sentences. ";

}  // namespace jarvis::prompts

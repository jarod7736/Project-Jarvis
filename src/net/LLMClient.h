#pragma once

// LLMClient — Phase 6 OpenClaw client.
//
// Hits an OpenAI-compatible /v1/chat/completions endpoint. Used by the
// IntentRouter for `local_llm` (small reasoning model on OpenClaw) and
// `claude` (Claude via the same gateway, model picked by the `model`
// field).
//
// Blocking. Caller must show "Thinking..." display state BEFORE invoking
// — loop() stalls during the round trip. Tier check belongs in the caller
// too: don't bother calling on OFFLINE or HOTSPOT_ONLY.

#include <Arduino.h>

namespace jarvis::net {

class LLMClient {
public:
    // True iff NVS has both oc_key and a host (default falls back). Cheap.
    static bool isConfigured();

    // POST /v1/chat/completions with the given user prompt. `model` selects
    // which backend (config::kOcLocalModel or kOcClaudeModel). Returns the
    // assistant's reply truncated to kOcMaxReplyChars at a sentence boundary,
    // or empty String on any failure (caller handles speak-error).
    static String query(const String& userPrompt, const char* model);
};

}  // namespace jarvis::net

#pragma once

// AnthropicClient — direct calls to api.anthropic.com /v1/messages.
//
// Used by the IntentRouter for the "claude" intent when the
// `anth_key` NVS field is set. Falls through to LLMClient (LM Studio)
// when the key is empty, so existing setups keep working unchanged.
//
// Wire shape:
//   POST https://api.anthropic.com/v1/messages
//   Headers:
//     x-api-key: <NVS anth_key>
//     anthropic-version: 2023-06-01
//     content-type: application/json
//   Body:
//     {
//       "model": "<NVS anth_model OR config default>",
//       "max_tokens": <kOcMaxTokens>,
//       "system": "Reply in one short spoken sentence. No analysis.",
//       "messages": [{"role": "user", "content": "<prompt>"}]
//     }
//   Response shape:
//     {
//       "id": "...",
//       "content": [{"type": "text", "text": "..."}, ...],
//       ...
//     }
//
// Different from the OpenAI-compat shape in LLMClient.cpp:
//   - Auth is x-api-key (not Bearer)
//   - System prompt is a top-level field, not a messages[] entry
//   - Reply text lives in content[N].text where type=="text", not
//     choices[0].message.content
//
// CLAUDE.md WiFiClientSecure rule applies — setInsecure() until cert
// pinning is implemented.

#include <Arduino.h>

namespace jarvis::net {

class AnthropicClient {
public:
    // True iff NVS has anth_key set (non-empty). Routing layer uses
    // this to decide between Anthropic direct vs LM Studio fallback
    // for the "claude" intent.
    static bool isConfigured();

    // Send userPrompt to Anthropic, return the reply text. Empty
    // string on any error (network, HTTP non-2xx, JSON parse,
    // empty content array). Caller speaks ERR_LLM_TIMEOUT or similar
    // on empty return.
    //
    // Strips "<think>...</think>" and the gemma-style meta-preamble
    // patterns just like LLMClient does — Claude doesn't emit those
    // by default, but the cleanup is cheap and keeps the post-
    // processing consistent across backends.
    static String query(const String& userPrompt);
};

}  // namespace jarvis::net

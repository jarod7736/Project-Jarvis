#include "AnthropicClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "../app/NVSConfig.h"
#include "../config.h"

namespace jarvis::net {

namespace {

constexpr const char* kHost          = "api.anthropic.com";
constexpr const char* kPath          = "/v1/messages";
constexpr const char* kApiVersion    = "2023-06-01";
constexpr const char* kSystemPrompt  = "Reply in one short spoken sentence. No analysis.";

// Claude is well-behaved — no <think> blocks, no gemma-style meta
// preamble. Just truncate to the same kOcMaxReplyChars budget so TTS
// pacing matches the local model's reply length.
String sentenceBoundaryTruncate(const String& s, size_t maxChars) {
    if (s.length() <= maxChars) return s;
    int cut = -1;
    for (int i = (int)maxChars - 1; i >= 0 && i > (int)maxChars / 2; --i) {
        char c = s[i];
        if (c == '.' || c == '?' || c == '!') { cut = i + 1; break; }
    }
    if (cut < 0) cut = maxChars;
    return s.substring(0, cut);
}

// Default model when NVS doesn't have one. Haiku 4.5 is the fast/cheap
// pick for short voice replies; the user can override via the captive
// portal `anth_model` field.
const char* defaultModel() {
    return "claude-haiku-4-5-20251001";
}

}  // namespace

bool AnthropicClient::isConfigured() {
    return jarvis::NVSConfig::getAnthKey().length() > 0;
}

String AnthropicClient::query(const String& userPrompt) {
    if (!isConfigured()) {
        Serial.println("[Anthropic] not configured (no anth_key)");
        return String();
    }

    String key   = jarvis::NVSConfig::getAnthKey();
    String model = jarvis::NVSConfig::getAnthModel();
    if (model.length() == 0) model = defaultModel();

    // Build request body. Anthropic Messages API: top-level `system`
    // field, `messages[]` with role+content. Single user turn — no
    // few-shot anchors needed; Claude follows the system prompt
    // reliably without them.
    String body;
    {
        JsonDocument req;
        req["model"]      = model;
        req["max_tokens"] = jarvis::config::kOcMaxTokens;
        req["system"]     = kSystemPrompt;
        auto messages     = req["messages"].to<JsonArray>();
        auto m            = messages.add<JsonObject>();
        m["role"]         = "user";
        m["content"]      = userPrompt;
        serializeJson(req, body);
    }

    Serial.printf("[Anthropic] POST https://%s%s model=%s prompt=%u chars\n",
                  kHost, kPath, model.c_str(), (unsigned)userPrompt.length());

    // Stack-allocated TLS client — same lifetime pattern as LLMClient.
    // setInsecure per CLAUDE.md (cert pinning is a deferred TODO).
    WiFiClientSecure secure;
    secure.setInsecure();

    HTTPClient http;
    http.setTimeout(jarvis::config::kOcHttpTimeoutMs);
    http.useHTTP10(true);

    String url = String("https://") + kHost + kPath;
    if (!http.begin(secure, url)) {
        Serial.println("[Anthropic] http.begin failed");
        return String();
    }
    http.addHeader("x-api-key",         key);
    http.addHeader("anthropic-version", kApiVersion);
    http.addHeader("content-type",      "application/json");

    int code = http.POST(body);
    if (code < 200 || code >= 300) {
        // Capture a short error body for diagnostics — Anthropic returns
        // useful detail (auth failures, model name typos, rate limits).
        String err = http.getString();
        if (err.length() > 240) err = err.substring(0, 240) + "...";
        Serial.printf("[Anthropic] HTTP %d  %s\n", code, err.c_str());
        http.end();
        return String();
    }
    String resp = http.getString();
    http.end();

    if (resp.length() == 0) return String();

    // Parse: content is an array of blocks; concatenate every
    // {type:"text", text:"..."} block's text. Tool-use blocks are
    // ignored (we don't expose tools to the model yet).
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (err) {
        Serial.printf("[Anthropic] JSON parse failed: %s\n", err.c_str());
        return String();
    }

    String content;
    JsonArray blocks = doc["content"].as<JsonArray>();
    for (JsonVariant blk : blocks) {
        if (blk["type"].as<String>() == "text") {
            if (content.length()) content += " ";
            content += blk["text"].as<String>();
        }
    }
    content.trim();
    if (content.length() == 0) {
        Serial.println("[Anthropic] empty content blocks");
        return String();
    }

    String trimmed = sentenceBoundaryTruncate(content, jarvis::config::kOcMaxReplyChars);
    Serial.printf("[Anthropic] -> %u chars (raw=%u)\n",
                  (unsigned)trimmed.length(), (unsigned)content.length());
    return trimmed;
}

}  // namespace jarvis::net

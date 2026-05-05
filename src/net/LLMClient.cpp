#include "LLMClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "../app/NVSConfig.h"
#include "../config.h"

namespace jarvis::net {

namespace {

String makeUrl(const String& host) {
    String url;
    if (host.startsWith("http://") || host.startsWith("https://")) {
        url = host;
    } else {
        url = "https://" + host;
    }
    url += "/v1/chat/completions";
    return url;
}

// Truncate `s` to at most `maxChars`, preferring the last sentence boundary
// (. ? !) before that limit. If no boundary in range, hard-cut at maxChars.
// Voice pacing means a half-sentence is much worse than a clipped paragraph.
String sentenceBoundaryTruncate(const String& s, size_t maxChars) {
    if (s.length() <= maxChars) return s;
    int cut = -1;
    for (int i = (int)maxChars - 1; i >= 0 && i > (int)maxChars / 2; --i) {
        char c = s[i];
        if (c == '.' || c == '?' || c == '!') {
            cut = i + 1;
            break;
        }
    }
    if (cut < 0) cut = maxChars;
    return s.substring(0, cut);
}

}  // namespace

bool LLMClient::isConfigured() {
    return jarvis::NVSConfig::getOcKey().length()  > 0 &&
           jarvis::NVSConfig::getOcHost().length() > 0;
}

String LLMClient::query(const String& userPrompt, const char* model) {
    if (!isConfigured()) {
        Serial.println("[LLMClient] not configured (no oc_key/host)");
        return String();
    }

    String host  = jarvis::NVSConfig::getOcHost();
    String token = jarvis::NVSConfig::getOcKey();
    String url   = makeUrl(host);

    // Build request body. Keep this small — Qwen 0.5B context is irrelevant
    // here (server-side model is a real reasoning model), but we still pay
    // for upload bytes over Tailscale and want predictable latency.
    String body;
    {
        JsonDocument req;
        req["model"]      = model;
        req["max_tokens"] = jarvis::config::kOcMaxTokens;
        req["stream"]     = false;
        auto messages     = req["messages"].to<JsonArray>();
        auto sys = messages.add<JsonObject>();
        sys["role"]    = "system";
        sys["content"] = "You are Jarvis, a concise voice assistant. "
                         "Reply in 1-3 sentences.";
        auto usr = messages.add<JsonObject>();
        usr["role"]    = "user";
        usr["content"] = userPrompt;
        serializeJson(req, body);
    }

    Serial.printf("[LLMClient] POST %s model=%s prompt=%u chars\n",
                  url.c_str(), model, (unsigned)userPrompt.length());

    // The user's OpenClaw is LM Studio over Tailscale, plain HTTP on the
    // Tailscale IP. The Nabu Casa default would be HTTPS. Branch on the
    // scheme so we use the right transport class.
    WiFiClient*       plain  = nullptr;
    WiFiClientSecure* secure = nullptr;
    HTTPClient http;
    http.setTimeout(jarvis::config::kOcHttpTimeoutMs);
    http.useHTTP10(true);          // disable chunked transfer (CLAUDE.md)

    bool ok;
    if (url.startsWith("https://")) {
        secure = new WiFiClientSecure();
        secure->setInsecure();     // cert pinning is TODO
        ok = http.begin(*secure, url);
    } else {
        plain = new WiFiClient();
        ok = http.begin(*plain, url);
    }
    if (!ok) {
        Serial.println("[LLMClient] http.begin failed");
        delete plain; delete secure;
        return String();
    }
    http.addHeader("Authorization", String("Bearer ") + token);
    http.addHeader("Content-Type",  "application/json");

    int code = http.POST(body);
    if (code < 200 || code >= 300) {
        Serial.printf("[LLMClient] HTTP %d\n", code);
        http.end();
        delete plain; delete secure;
        return String();
    }

    String resp = http.getString();
    http.end();
    delete plain; delete secure;

    // Parse OpenAI-compat response: choices[0].message.content
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (err) {
        Serial.printf("[LLMClient] JSON parse failed: %s\n", err.c_str());
        return String();
    }

    String content = doc["choices"][0]["message"]["content"].as<String>();
    content.trim();
    if (content.length() == 0) {
        Serial.println("[LLMClient] empty content in response");
        return String();
    }

    String trimmed = sentenceBoundaryTruncate(content, jarvis::config::kOcMaxReplyChars);
    Serial.printf("[LLMClient] -> %u chars (truncated from %u)\n",
                  (unsigned)trimmed.length(), (unsigned)content.length());
    return trimmed;
}

}  // namespace jarvis::net

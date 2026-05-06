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

// Strip <think>...</think> blocks some chat models (gemma-4 included) emit
// when they decide to deliberate out loud despite the system prompt.
// Best-effort — non-recursive, takes first <think>...</think> if present.
String stripThinkingMarkers(const String& s) {
    int open = s.indexOf("<think>");
    if (open < 0) return s;
    int close = s.indexOf("</think>", open);
    if (close < 0) return s;
    String out = s.substring(0, open) + s.substring(close + 8);
    out.trim();
    return out;
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

// Template helper so the WiFi*Client lives on the stack for the duration of
// the request and gets cleaned up in defined order. The previous heap-new
// pattern crashed with LoadProhibited on response return — HTTPClient was
// still holding a pointer to the client we'd just deleted.
template<typename Client>
static String doPostImpl(Client& client, const String& url, const String& body,
                         const String& token) {
    HTTPClient http;
    http.setTimeout(jarvis::config::kOcHttpTimeoutMs);
    http.useHTTP10(true);

    if (!http.begin(client, url)) {
        Serial.println("[LLMClient] http.begin failed");
        return String();
    }
    http.addHeader("Authorization", String("Bearer ") + token);
    http.addHeader("Content-Type",  "application/json");

    int code = http.POST(body);
    if (code < 200 || code >= 300) {
        Serial.printf("[LLMClient] HTTP %d\n", code);
        http.end();
        return String();
    }
    String resp = http.getString();
    http.end();
    return resp;
}

String LLMClient::query(const String& userPrompt, const char* model) {
    if (!isConfigured()) {
        Serial.println("[LLMClient] not configured (no oc_key/host)");
        return String();
    }

    String host  = jarvis::NVSConfig::getOcHost();
    String token = jarvis::NVSConfig::getOcKey();
    String url   = makeUrl(host);

    // Build request body. Keep this small — payload size affects latency
    // over the LAN/Tailscale link.
    String body;
    {
        JsonDocument req;
        req["model"]      = model;
        req["max_tokens"] = jarvis::config::kOcMaxTokens;
        req["stream"]     = false;
        auto messages     = req["messages"].to<JsonArray>();
        auto sys = messages.add<JsonObject>();
        sys["role"]    = "system";
        sys["content"] = "You are Jarvis, a concise voice assistant. Reply "
                         "directly in 1-3 sentences. Plain prose only — no "
                         "lists, no markdown, no headers, no meta commentary, "
                         "no analysis of the question.";
        // Few-shot anchor — gemma-4 ignores the system prompt's "no meta"
        // instruction on its own, but follows the format from a concrete
        // example. One shot is enough to suppress the bullet-list/analysis
        // pattern.
        auto ex_u = messages.add<JsonObject>();
        ex_u["role"]    = "user";
        ex_u["content"] = "what's the capital of france";
        auto ex_a = messages.add<JsonObject>();
        ex_a["role"]    = "assistant";
        ex_a["content"] = "Paris is the capital of France.";

        auto usr = messages.add<JsonObject>();
        usr["role"]    = "user";
        usr["content"] = userPrompt;
        serializeJson(req, body);
    }

    Serial.printf("[LLMClient] POST %s model=%s prompt=%u chars\n",
                  url.c_str(), model, (unsigned)userPrompt.length());

    // Stack-allocate the right client type based on URL scheme. The client
    // and its HTTPClient stay alive together inside doPostImpl; both
    // destruct in well-defined order on return.
    String resp;
    if (url.startsWith("https://")) {
        WiFiClientSecure secure;
        secure.setInsecure();        // cert pinning is TODO
        resp = doPostImpl(secure, url, body, token);
    } else {
        WiFiClient plain;
        resp = doPostImpl(plain, url, body, token);
    }
    if (resp.length() == 0) return String();

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

    String stripped = stripThinkingMarkers(content);
    String trimmed  = sentenceBoundaryTruncate(stripped, jarvis::config::kOcMaxReplyChars);
    Serial.printf("[LLMClient] -> %u chars (raw=%u, post-strip=%u)\n",
                  (unsigned)trimmed.length(),
                  (unsigned)content.length(),
                  (unsigned)stripped.length());
    return trimmed;
}

}  // namespace jarvis::net

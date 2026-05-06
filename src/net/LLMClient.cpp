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

// Heuristic: gemma-4-e4b emits a meta preamble before (or instead of) the
// answer — sentences narrating the model's own reasoning. Drop leading
// sentences that match any of these markers; keep from the first non-meta
// sentence on. If everything is meta, return the original (speaking
// awkward text beats speaking nothing).
bool startsWithMeta(const String& sentence) {
    String s = sentence;
    s.trim();
    if (s.length() == 0) return true;
    String head = s.substring(0, s.length() < 48 ? s.length() : 48);
    head.toLowerCase();
    // Generic prefixes catch most gemma-4-e4b inner-monologue patterns.
    // "the user " covers all of: asking, wants, asked, provided, said,
    // gave, input, has, is, etc. without the brittle list of inflections.
    // Same for "i " (followed by a planning verb).
    static const char* markers[] = {
        "the user ",           "the input ",         "the question ",
        "the prompt ",         "the response should","the answer should",
        "i need to",           "i should",           "i must",
        "i am processing",     "i am to",            "i'll ",
        "i will ",             "i can ",             "i'm going to",
        "i already",           "i'm just",
        "since the input",     "since the user",     "given the user",
        "given that",          "let me analyze",     "let me think",
        "okay, the user",      "okay,",              "alright,",
        "first, i",            "to answer this",     "to address this",
        "my function is",      "my purpose is",      "my role is",
        "as jarvis,",          "this requires",      "based on the user",
        "based on the input",
    };
    for (auto* m : markers) {
        if (head.startsWith(m)) return true;
    }
    return false;
}

// Walk every sentence and keep only the non-meta ones. gemma-4-e4b
// sometimes interleaves analysis with the answer ("I already explained it
// correctly. I should give a concise version. <actual answer>"), so
// stripping just the preamble leaves enough garbage to ruin the spoken
// reply. This is the broader "drop meta wherever it appears" pass.
String stripMetaPreamble(const String& s) {
    if (s.length() == 0) return s;
    String out;
    out.reserve(s.length());
    int start = 0;
    auto flush_sentence = [&](int end) {
        if (end <= start) return;
        String sentence = s.substring(start, end);
        if (!startsWithMeta(sentence)) {
            if (out.length()) out += ' ';
            String trimmed = sentence;
            trimmed.trim();
            out += trimmed;
        }
    };
    for (int i = 0; i < (int)s.length(); ++i) {
        char c = s[i];
        if (c == '.' || c == '!' || c == '?') {
            int end = i + 1;
            flush_sentence(end);
            // Skip whitespace after terminator so the next sentence's
            // start position is clean.
            while (end < (int)s.length() && (s[end] == ' ' || s[end] == '\n')) ++end;
            start = end;
            i = end - 1;
        }
    }
    // Trailing fragment without terminator (rare in normal text but
    // possible if the response was truncated mid-sentence).
    if (start < (int)s.length()) flush_sentence(s.length());

    out.trim();
    if (out.length() == 0) {
        // Every sentence in the response was meta — the model never gave
        // an actual answer. Speak a graceful fallback instead of empty
        // silence (user gets confused) or the inner monologue.
        return String("Sorry, I didn't quite catch that. Try again?");
    }
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
        // Drastically simplified system prompt. The longer version made
        // gemma-4-e4b deliberate ABOUT the prompt itself ("the response
        // format is...", "I should respect all constraints..."). Short
        // prompts give thinking models less to chew on.
        sys["content"] = "Reply in one short spoken sentence. No analysis.";
        // Two tight few-shot anchors. More than that fed gemma's tendency
        // to model the "format" as a topic. These are deliberately plain:
        // the assistant message is a direct sentence with no preamble.
        struct Shot { const char* u; const char* a; };
        static const Shot shots[] = {
            {"what's the capital of france", "Paris is the capital of France."},
            {"explain photosynthesis",
             "Plants use sunlight, water, and carbon dioxide to make sugar "
             "and release oxygen."},
        };
        for (auto& s : shots) {
            auto eu = messages.add<JsonObject>();
            eu["role"]    = "user";
            eu["content"] = s.u;
            auto ea = messages.add<JsonObject>();
            ea["role"]    = "assistant";
            ea["content"] = s.a;
        }

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

    String stripped  = stripThinkingMarkers(content);
    String demeta    = stripMetaPreamble(stripped);
    String trimmed   = sentenceBoundaryTruncate(demeta, jarvis::config::kOcMaxReplyChars);
    Serial.printf("[LLMClient] -> %u chars (raw=%u, post-think=%u, post-meta=%u)\n",
                  (unsigned)trimmed.length(),
                  (unsigned)content.length(),
                  (unsigned)stripped.length(),
                  (unsigned)demeta.length());
    return trimmed;
}

}  // namespace jarvis::net

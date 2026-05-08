#include "TtsClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "../app/NVSConfig.h"
#include "../config.h"

namespace jarvis::net {

namespace {

// Allocate `n` bytes in PSRAM if available, else regular heap. The CoreS3
// has 8 MB OPI PSRAM (CLAUDE.md PSRAM rule) so any buffer ≥ a couple KB
// belongs there — keeps internal SRAM free for stack and DMA.
uint8_t* psramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return reinterpret_cast<uint8_t*>(p);
    // Fallback if PSRAM disabled or saturated — better to allocate from
    // internal heap and squeeze than to silently return nothing.
    return reinterpret_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_8BIT));
}

// Build the OpenAI request body. `instructions` is a prosody hint
// supported only by `gpt-4o-mini-tts` — `tts-1` and `tts-1-hd` reject
// the field with a 400, so we only emit it when the model name starts
// with "gpt-4o". Empty `instructions` is also omitted regardless of
// model (no point sending an empty string).
String buildOpenAiBody(const String& text, const String& voice,
                       const String& model, const String& instructions) {
    JsonDocument req;
    req["model"]           = model;
    req["voice"]           = voice;
    req["input"]           = text;
    req["response_format"] = "mp3";
    if (instructions.length() > 0 && model.startsWith("gpt-4o")) {
        req["instructions"] = instructions;
    }
    String body;
    serializeJson(req, body);
    return body;
}

// ElevenLabs body. Their API takes `text` plus a `voice_settings` object
// for stability/similarity tuning. Use the model_id query (sent as a
// JSON field) and request mp3_44100_128 (their highest quality MP3).
String buildElevenBody(const String& text, const String& model) {
    JsonDocument req;
    req["text"]     = text;
    req["model_id"] = model;
    auto vs = req["voice_settings"].to<JsonObject>();
    vs["stability"]        = 0.5;
    vs["similarity_boost"] = 0.75;
    String body;
    serializeJson(req, body);
    return body;
}

// Common: download `http`'s response body into a freshly-allocated PSRAM
// buffer, capped at kTtsMaxMp3Bytes. Returns the buffer or empty on any
// failure. http is consumed (end()'d) by the caller.
Mp3Buffer downloadBody(HTTPClient& http, int code) {
    Mp3Buffer out;
    if (code < 200 || code >= 300) {
        Serial.printf("[TtsClient] HTTP %d\n", code);
        return out;
    }
    int contentLen = http.getSize();  // -1 if chunked / unknown
    size_t cap = jarvis::config::kTtsMaxMp3Bytes;
    if (contentLen > 0) {
        if ((size_t)contentLen > cap) {
            Serial.printf("[TtsClient] response too large: %d > %u cap\n",
                          contentLen, (unsigned)cap);
            return out;
        }
        cap = (size_t)contentLen;
    }

    uint8_t* buf = psramAlloc(cap);
    if (!buf) {
        Serial.printf("[TtsClient] PSRAM alloc failed (%u bytes)\n",
                      (unsigned)cap);
        return out;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t lastByte = millis();
    while (got < cap) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf + got,
                                      std::min((size_t)avail, cap - got));
            if (n > 0) {
                got += n;
                lastByte = millis();
            }
        } else {
            // No bytes ready. If the server closed the connection AND
            // we've drained everything, we're done. Otherwise idle.
            if (!http.connected() && stream->available() == 0) break;
            if (millis() - lastByte > 3000) {
                Serial.println("[TtsClient] body read stall (>3s)");
                break;
            }
            delay(5);
        }
    }

    if (got == 0) {
        Serial.println("[TtsClient] empty body");
        free(buf);
        return out;
    }
    Serial.printf("[TtsClient] downloaded %u bytes\n", (unsigned)got);
    out.data.reset(buf);
    out.length = got;
    return out;
}

Mp3Buffer synthOpenAi(const String& text, const String& voice,
                      const String& model, const String& apiKey,
                      const String& instructions) {
    using namespace jarvis::config;

    WiFiClientSecure secure;
    secure.setInsecure();  // cert pinning is TODO across the project
    HTTPClient http;
    http.setTimeout(kTtsHttpTimeoutMs);
    http.useHTTP10(true);

    String url = String("https://") + kTtsOpenAIHost + kTtsOpenAIPath;
    if (!http.begin(secure, url)) {
        Serial.println("[TtsClient] http.begin failed (openai)");
        return Mp3Buffer{};
    }
    http.addHeader("Authorization", String("Bearer ") + apiKey);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("Accept",        "audio/mpeg");

    String body = buildOpenAiBody(text, voice, model, instructions);
    Serial.printf("[TtsClient] POST %s body=%u chars\n", url.c_str(),
                  (unsigned)body.length());
    int code = http.POST(body);
    Mp3Buffer out = downloadBody(http, code);
    http.end();
    return out;
}

Mp3Buffer synthEleven(const String& text, const String& voice,
                      const String& model, const String& apiKey) {
    using namespace jarvis::config;

    WiFiClientSecure secure;
    secure.setInsecure();
    HTTPClient http;
    http.setTimeout(kTtsHttpTimeoutMs);
    http.useHTTP10(true);

    String url = String("https://") + kTtsElevenHost + kTtsElevenPathBase + voice;
    if (!http.begin(secure, url)) {
        Serial.println("[TtsClient] http.begin failed (eleven)");
        return Mp3Buffer{};
    }
    http.addHeader("xi-api-key",   apiKey);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept",       "audio/mpeg");

    String body = buildElevenBody(text, model);
    Serial.printf("[TtsClient] POST %s body=%u chars\n", url.c_str(),
                  (unsigned)body.length());
    int code = http.POST(body);
    Mp3Buffer out = downloadBody(http, code);
    http.end();
    return out;
}

}  // namespace

bool TtsClient::isConfigured() {
    return jarvis::NVSConfig::getTtsApiKey().length() > 0;
}

Mp3Buffer TtsClient::synthesize(const String& text) {
    if (text.length() == 0) return Mp3Buffer{};
    if (!isConfigured()) {
        // Caller already checked tts_provider != "melotts" before calling
        // us, so reaching here with no key is a config error worth a log.
        Serial.println("[TtsClient] not configured (no tts_api_key)");
        return Mp3Buffer{};
    }

    String provider     = jarvis::NVSConfig::getTtsProvider();
    String voice        = jarvis::NVSConfig::getTtsVoiceId();
    String model        = jarvis::NVSConfig::getTtsModel();
    String apiKey       = jarvis::NVSConfig::getTtsApiKey();
    String instructions = jarvis::NVSConfig::getTtsInstructions();

    Serial.printf("[TtsClient] synth provider=%s voice=%s model=%s instr=%u len=%u\n",
                  provider.c_str(), voice.c_str(), model.c_str(),
                  (unsigned)instructions.length(), (unsigned)text.length());

    if (provider.equalsIgnoreCase("openai")) {
        return synthOpenAi(text, voice, model, apiKey, instructions);
    }
    if (provider.equalsIgnoreCase("eleven") ||
        provider.equalsIgnoreCase("elevenlabs")) {
        return synthEleven(text, voice, model, apiKey);
    }
    Serial.printf("[TtsClient] unsupported provider \"%s\"\n", provider.c_str());
    return Mp3Buffer{};
}

}  // namespace jarvis::net

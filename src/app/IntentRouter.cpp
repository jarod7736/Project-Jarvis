#include "IntentRouter.h"

#include <ArduinoJson.h>

#include "../config.h"
#include "../hal/LLMModule.h"
#include "../net/AnthropicClient.h"
#include "../net/LLMClient.h"
#include "../net/OtaService.h"
#include "../net/WiFiManager.h"
#include "CommandHandler.h"
#include "MathParser.h"
#include "NVSConfig.h"

namespace jarvis::app {

namespace {

jarvis::hal::LLMModule* g_module = nullptr;

// Qwen 0.5B frequently wraps JSON in markdown backticks. CLAUDE.md flags
// this. Strip leading ```json / ``` and trailing ``` before parsing.
String stripJsonMarkdown(const String& s) {
    String t = s;
    t.trim();
    if (t.startsWith("```json")) t.remove(0, 7);
    else if (t.startsWith("```"))  t.remove(0, 3);
    if (t.endsWith("```"))         t.remove(t.length() - 3);
    t.trim();
    return t;
}

// (Phase 5 v2) The system prompt is set at llm.setup() time per the M5
// firmware contract. queryLlm sends only the user transcript — Qwen sees
// it as the next "User: " line in the conversation we primed at setup.
// No buildIntentPrompt needed.

// On-device handlers. Phase 5 implements time/date; timer/reminder/math are
// stubbed with a polite fallback so the FSM doesn't dead-end.
RouteResult handleOnDevice(const String& query) {
    String q = query;
    q.toLowerCase();

    if (q.indexOf("time") >= 0 || q.indexOf("clock") >= 0) {
        struct tm t;
        if (!getLocalTime(&t, /*timeoutMs=*/200)) {
            return {false, String("I don't have the time yet — NTP isn't synced.")};
        }
        char buf[32];
        strftime(buf, sizeof(buf), "It's %I:%M %p.", &t);
        // %I has a leading zero on Windows-style strftime; trim.
        if (buf[3] == '0' && buf[4] != ':') {
            // never happens, but defensive
        }
        return {true, String(buf)};
    }
    if (q.indexOf("date") >= 0 || q.indexOf("day") >= 0) {
        struct tm t;
        if (!getLocalTime(&t, /*timeoutMs=*/200)) {
            return {false, String("I don't have the date yet — NTP isn't synced.")};
        }
        char buf[64];
        strftime(buf, sizeof(buf), "Today is %A, %B %e.", &t);
        return {true, String(buf)};
    }

    // Math — single binary op like "what is twelve plus seventeen".
    // MathParser handles words-to-numbers, operator detection, and the
    // computation; the router's job here is just to format the result.
    if (looksLikeMath(q)) {
        MathResult m = parseAndEvaluate(q);
        if (m.ok) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld.", (long long)m.value);
            return {true, String(buf)};
        }
        if (m.error.length() > 0) {
            return {false, m.error};
        }
        // Operator was present but parsing failed — fall through to
        // the "coming soon" stub below so the user still gets feedback.
    }

    // Timer / reminder not yet implemented.
    return {false, String("On-device timers are coming soon.")};
}

// Keyword-based intent classification. Qwen 0.5B (prefill-20e variant on
// this firmware) doesn't reliably follow instructions, so we rely on this
// as the primary path. Order: HA-command keywords first (most specific),
// then on_device markers, then local_llm/claude phrasings, finally
// CommandHandler's keyword table for any HA intents the prefix-match below
// doesn't catch.
const char* classifyByKeyword(const String& lc) {
    // Firmware update — must come before HA verbs since "update" is too
    // generic to leak into the LLM fallback. Two-word phrases only, so
    // routine HA chatter doesn't trigger.
    if (lc.indexOf("update firmware") >= 0 ||
        lc.indexOf("install update")  >= 0 ||
        lc.indexOf("update yourself") >= 0) {
        return "update_fw";
    }

    // HA control verbs — clearly a command, even if the entity isn't in our
    // table.
    if (lc.indexOf("turn on")    >= 0 ||
        lc.indexOf("turn off")   >= 0 ||
        lc.indexOf("lock ")      >= 0 ||
        lc.indexOf("unlock ")    >= 0 ||
        lc.indexOf("close the")  >= 0 ||
        lc.indexOf("open the")   >= 0) {
        return "ha_command";
    }

    // HA state queries
    if (lc.startsWith("is the ") ||
        lc.indexOf("temperature") >= 0 ||
        lc.indexOf("how warm")    >= 0 ||
        lc.indexOf("how hot")     >= 0 ||
        lc.indexOf("how cold")    >= 0) {
        return "ha_query";
    }

    // On-device: clock, date, timer
    if (lc.indexOf("what time")  >= 0 ||
        lc.indexOf("what's the time") >= 0 ||
        lc.indexOf("current time") >= 0 ||
        lc.indexOf("what day")   >= 0 ||
        lc.indexOf("what date")  >= 0 ||
        lc.indexOf("today's date") >= 0 ||
        lc.indexOf("set a timer") >= 0 ||
        lc.indexOf("set timer")  >= 0 ||
        lc.indexOf("remind me")  >= 0) {
        return "on_device";
    }

    // Math also goes on-device. Detect by operator words ("plus",
    // "minus", "times", "divided by", etc.) — these phrases all start
    // with "what is"/"what's"/"calculate" and the local_llm block
    // below catches "what is", so the math check has to come first.
    if (looksLikeMath(lc)) {
        return "on_device";
    }

    // Creative / nuanced — Claude territory
    if (lc.indexOf("write me")   >= 0 ||
        lc.indexOf("compose")    >= 0 ||
        lc.indexOf("haiku")      >= 0 ||
        lc.indexOf("poem")       >= 0 ||
        lc.indexOf("tell me a story") >= 0) {
        return "claude";
    }

    // Reasoning / factual — local LLM
    if (lc.indexOf("explain")    >= 0 ||
        lc.indexOf("what is")    >= 0 ||
        lc.indexOf("why does")   >= 0 ||
        lc.indexOf("how does")   >= 0 ||
        lc.indexOf("tell me about") >= 0) {
        return "local_llm";
    }

    return nullptr;  // unknown
}

RouteResult dispatchByIntent(const String& transcript, const String& intent,
                             const String& entity, const String& query,
                             jarvis::net::ConnectivityTier tier) {
    if (intent == "ha_command" || intent == "ha_query") {
        // Use CommandHandler for entity resolution — Qwen's entity field
        // is at best a hint; the keyword table is what maps to the user's
        // actual entity_ids.
        CommandResult c = jarvis::app::dispatch(transcript);
        if (c.handled) return {c.ok, c.spoken};
        return {false, String("I don't know that device yet.")};
    }
    if (intent == "on_device") {
        return handleOnDevice(query.length() ? query : transcript);
    }
    if (intent == "update_fw") {
        String url = jarvis::NVSConfig::getFwUrl();
        if (url.length() == 0) {
            return {false, String(jarvis::config::kOtaSpeakNoUrl)};
        }
        if (tier == jarvis::net::ConnectivityTier::OFFLINE) {
            return {false, String(jarvis::config::kErrNoNetwork)};
        }
        // Speak the start message asynchronously through LLMModule, then
        // block on HTTPUpdate. On success the device reboots inside
        // pullRemote(); on failure we fall through and the FSM speaks
        // kOtaSpeakFailed via the returned RouteResult.
        if (g_module) {
            g_module->speak(jarvis::config::kOtaSpeakStart);
            // Brief pause so TTS starts playing before HTTPUpdate hogs
            // the WiFi stack. 2s is well under the 30s loop watchdog and
            // the HTTPUpdate progress callback feeds the dog from there.
            delay(2000);
        }
        // pullRemote returns only on failure — success path reboots
        // inside HTTPUpdate. Discard the bool; failure is the only case.
        (void)jarvis::net::OtaService::pullRemote(url);
        return {false, String(jarvis::config::kOtaSpeakFailed)};
    }
    if (intent == "local_llm" || intent == "claude") {
        const String& prompt = query.length() ? query : transcript;
        String reply;

        // Two backends with different reachability requirements:
        //   - AnthropicClient hits api.anthropic.com — a public
        //     internet endpoint. Works on any non-OFFLINE tier
        //     (LAN, TAILSCALE, or HOTSPOT_ONLY all have internet).
        //   - LLMClient (OpenClaw / LM Studio) lives on the home LAN
        //     or via Tailscale. HOTSPOT_ONLY can't reach it, so we
        //     bail with the canned "brain" response instead of
        //     burning 5+ seconds on a doomed HTTP attempt.
        if (intent == "claude" && jarvis::net::AnthropicClient::isConfigured()) {
            if (tier == jarvis::net::ConnectivityTier::OFFLINE) {
                return {false, String("I can't reach my brain right now.")};
            }
            reply = jarvis::net::AnthropicClient::query(prompt);
        } else {
            if (tier != jarvis::net::ConnectivityTier::LAN &&
                tier != jarvis::net::ConnectivityTier::TAILSCALE) {
                return {false, String("I can't reach my brain right now.")};
            }
            const char* model = (intent == "claude")
                                    ? jarvis::config::kOcClaudeModel
                                    : jarvis::config::kOcLocalModel;
            reply = jarvis::net::LLMClient::query(prompt, model);
        }

        if (reply.length() == 0) {
            return {false, String("I can't reach my brain right now.")};
        }
        return {true, reply};
    }
    return {false, String(jarvis::config::kErrIntentParse)};
}

// Fall-through path when LLM is unavailable or JSON parsing fails. Try
// keyword classification first; on miss, try CommandHandler's table
// directly (catches phrasings the keyword classifier doesn't).
RouteResult fallbackToKeyword(const String& transcript,
                              jarvis::net::ConnectivityTier tier) {
    String lc = transcript;
    lc.toLowerCase();
    const char* intent = classifyByKeyword(lc);
    if (intent) {
        Serial.printf("[IntentRouter] keyword classified as %s\n", intent);
        return dispatchByIntent(transcript, String(intent), String(), String(), tier);
    }

    // No keyword hit. Try CommandHandler's table — it catches phrasings the
    // keyword classifier doesn't.
    CommandResult c = jarvis::app::dispatch(transcript);
    if (c.handled) return {c.ok, c.spoken};

    // Still nothing. Last useful move before giving up: pretend it's
    // local_llm and let gemma answer. ASR misrecognitions and unusual
    // phrasings produce more value as "let the smart model figure it out"
    // than "I wasn't sure what you meant." Tier-gated — OFFLINE still
    // bails out.
    if (tier != jarvis::net::ConnectivityTier::OFFLINE &&
        jarvis::net::LLMClient::isConfigured()) {
        Serial.println("[IntentRouter] no local match, asking OpenClaw");
        return dispatchByIntent(transcript, String("local_llm"),
                                String(), String(), tier);
    }
    return {false, String(jarvis::config::kErrIntentParse)};
}

}  // namespace

void intentRouterBegin(jarvis::hal::LLMModule* module) {
    g_module = module;
}

RouteResult route(const String& transcript, jarvis::net::ConnectivityTier tier) {
    if (transcript.length() == 0) {
        return {false, String(jarvis::config::kErrIntentParse)};
    }

    // qwen2.5-0.5b-prefill-20e is a prefill-optimized variant — it doesn't
    // reliably follow instruction-following prompts. We try Qwen as a
    // best-effort hint, then fall back to keyword classification which is
    // far more reliable for our 5-intent space.
    if (g_module && g_module->hasLlm()) {
        Serial.printf("[IntentRouter] querying Qwen with transcript (%u chars)\n",
                      (unsigned)transcript.length());
        String raw = g_module->queryLlm(transcript, /*timeoutMs=*/4000);
        Serial.printf("[IntentRouter] qwen raw: \"%s\"\n", raw.c_str());

        if (raw.length() > 0) {
            String stripped = stripJsonMarkdown(raw);
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, stripped);
            if (!err) {
                String intent = doc["intent"].as<String>();
                String entity = doc["entity"].as<String>();
                String query  = doc["query"].as<String>();
                if (intent.length() > 0) {
                    Serial.printf("[IntentRouter] qwen intent=%s\n", intent.c_str());
                    return dispatchByIntent(transcript, intent, entity, query, tier);
                }
            }
            Serial.println("[IntentRouter] Qwen output unparseable, using keywords");
        }
    }

    return fallbackToKeyword(transcript, tier);
}

}  // namespace jarvis::app

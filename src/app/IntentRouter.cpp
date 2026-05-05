#include "IntentRouter.h"

#include <ArduinoJson.h>

#include "../config.h"
#include "../hal/LLMModule.h"
#include "../net/LLMClient.h"
#include "../net/WiFiManager.h"
#include "CommandHandler.h"

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

    // Timer / reminder / math not yet implemented.
    return {false, String("On-device timers and math are coming soon.")};
}

// Keyword-based intent classification. Qwen 0.5B (prefill-20e variant on
// this firmware) doesn't reliably follow instructions, so we rely on this
// as the primary path. Order: HA-command keywords first (most specific),
// then on_device markers, then local_llm/claude phrasings, finally
// CommandHandler's keyword table for any HA intents the prefix-match below
// doesn't catch.
const char* classifyByKeyword(const String& lc) {
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
    if (intent == "local_llm" || intent == "claude") {
        // Phase 6: dispatch to OpenClaw. Tier-gate first — Tailscale-only
        // backend, so OFFLINE/HOTSPOT_ONLY can't reach it.
        if (tier == jarvis::net::ConnectivityTier::OFFLINE) {
            return {false, String(jarvis::config::kErrNoNetwork)};
        }
        const char* model = (intent == "claude")
                                ? jarvis::config::kOcClaudeModel
                                : jarvis::config::kOcLocalModel;
        String reply = jarvis::net::LLMClient::query(
            query.length() ? query : transcript, model);
        if (reply.length() == 0) {
            return {false, String(jarvis::config::kErrLlmTimeout)};
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
    if (!intent) {
        // Last resort: try CommandHandler — it has the entity table that
        // recognizes phrasings the keyword classifier doesn't (e.g. "is the
        // garage open" if startsWith fails on punctuation).
        CommandResult c = jarvis::app::dispatch(transcript);
        if (c.handled) return {c.ok, c.spoken};
        return {false, String(jarvis::config::kErrIntentParse)};
    }
    Serial.printf("[IntentRouter] keyword classified as %s\n", intent);
    return dispatchByIntent(transcript, String(intent), String(), String(), tier);
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

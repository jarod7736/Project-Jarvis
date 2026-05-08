#include "ConfigSchema.h"

#include <Preferences.h>

#include "../hal/AudioPlayer.h"
#include "../hal/Display.h"
#include "NVSConfig.h"

namespace jarvis::app {

namespace {

enum class FieldType : uint8_t { Bool, Int, String, Enum };

struct EnumOption { const char* value; const char* label; };

struct ConfigField {
    const char* key;        // NVS key + JSON key
    const char* label;      // Human-facing
    const char* category;   // UI tab: audio / routing / network / display
    FieldType   type;
    bool        sensitive;  // mask in UI, never log
    // Validation
    int         imin, imax;
    // Defaults (only one is meaningful per type)
    int         idefault;
    bool        bdefault;
    const char* sdefault;
    // Enum metadata
    const EnumOption* options;
    size_t            optionCount;
};

constexpr EnumOption kTierOptions[] = {
    {"auto",  "Auto (route via Qwen)"},
    {"local", "Local LLM only"},
    {"cloud", "Force Claude"},
    {"qwen",  "Qwen only (offline)"},
};
constexpr EnumOption kTtsProviderOptions[] = {
    {"melotts", "Local (melotts)"},
    {"openai",  "OpenAI"},
    {"eleven",  "ElevenLabs"},
};

constexpr ConfigField kSchema[] = {
    // ── Audio ───────────────────────────────────────────────────────────
    {"tts_volume",   "TTS Volume",         "audio",   FieldType::Int,
     false, 0, 100, 70, false, nullptr,  nullptr, 0},
    {"wake_sens",    "Wake Sensitivity",   "audio",   FieldType::Int,
     false, 1,  10,  5, false, nullptr,  nullptr, 0},
    {"mic_gain",     "Mic Gain",           "audio",   FieldType::Int,
     false, 0, 100, 50, false, nullptr,  nullptr, 0},

    // ── Routing ─────────────────────────────────────────────────────────
    {"default_tier", "Default Tier",       "routing", FieldType::Enum,
     false, 0, 0, 0, false, "auto",
     kTierOptions, sizeof(kTierOptions) / sizeof(kTierOptions[0])},
    {"route_timeout", "Route Timeout (ms)","routing", FieldType::Int,
     false, 500, 10000, 3000, false, nullptr, nullptr, 0},
    {"log_to_sd",    "Log to SD card",     "routing", FieldType::Bool,
     false, 0, 0, 0, true, nullptr, nullptr, 0},

    // ── Endpoints ───────────────────────────────────────────────────────
    {"ha_host",      "Home Assistant Host","network", FieldType::String,
     false, 0, 0, 0, false, "", nullptr, 0},
    {"ha_token",     "HA Long-Lived Token","network", FieldType::String,
     true,  0, 0, 0, false, "", nullptr, 0},
    {"oc_host",      "OpenClaw URL",       "network", FieldType::String,
     false, 0, 0, 0, false, "", nullptr, 0},
    {"oc_key",       "OpenClaw Key",       "network", FieldType::String,
     true,  0, 0, 0, false, "", nullptr, 0},

    // ── TTS provider ────────────────────────────────────────────────────
    {"tts_provider", "TTS Provider",       "network", FieldType::Enum,
     false, 0, 0, 0, false, "melotts",
     kTtsProviderOptions, sizeof(kTtsProviderOptions) / sizeof(kTtsProviderOptions[0])},
    {"tts_voice_id", "TTS Voice ID",       "network", FieldType::String,
     false, 0, 0, 0, false, "onyx", nullptr, 0},
    {"tts_api_key",  "TTS API Key",        "network", FieldType::String,
     true,  0, 0, 0, false, "", nullptr, 0},
    {"tts_instr",    "TTS Prosody Hint",   "network", FieldType::String,
     false, 0, 0, 0, false, "", nullptr, 0},

    // ── MQTT ────────────────────────────────────────────────────────────
    {"mqtt_host",    "MQTT Broker Host",   "network", FieldType::String,
     false, 0, 0, 0, false, "", nullptr, 0},
    {"mqtt_user",    "MQTT User",          "network", FieldType::String,
     false, 0, 0, 0, false, "", nullptr, 0},
    {"mqtt_pass",    "MQTT Password",      "network", FieldType::String,
     true,  0, 0, 0, false, "", nullptr, 0},

    // ── Anthropic API direct (claude intent) ────────────────────────────
    // When `anth_key` is set, IntentRouter routes the "claude" intent to
    // api.anthropic.com directly via net/AnthropicClient. Empty key
    // falls back to the OpenAI-compat path (LM Studio / OpenClaw).
    {"anth_key",     "Anthropic API Key",  "network", FieldType::String,
     true,  0, 0, 0, false, "", nullptr, 0},
    {"anth_model",   "Anthropic Model",    "network", FieldType::String,
     false, 0, 0, 0, false, "claude-haiku-4-5-20251001", nullptr, 0},

    // ── OTA ─────────────────────────────────────────────────────────────
    {"fw_url",       "Firmware Update URL","network", FieldType::String,
     false, 0, 0, 0, false, "", nullptr, 0},
    {"ota_pass",     "OTA Password",       "network", FieldType::String,
     true,  0, 0, 0, false, "", nullptr, 0},

    // ── Display ─────────────────────────────────────────────────────────
    {"brightness",   "Display Brightness", "display", FieldType::Int,
     false, 10, 255, 180, false, nullptr, nullptr, 0},
    {"sleep_secs",   "Sleep After (sec)",  "display", FieldType::Int,
     false,  0, 600,  60, false, nullptr, nullptr, 0},
    {"hold_ms",      "Long-Press (ms)",    "display", FieldType::Int,
     false, 500, 5000, 2000, false, nullptr, nullptr, 0},
    {"hold_slack",   "Press Slack (ms)",   "display", FieldType::Int,
     false,   0,  500,  150, false, nullptr, nullptr, 0},
};
constexpr size_t kSchemaCount = sizeof(kSchema) / sizeof(kSchema[0]);

constexpr const char* NS = "jarvis";

// ── Generic NVS reader/writer (for fields without a dedicated NVSConfig
// accessor). Reads/writes go straight to the "jarvis" namespace.
int readInt(const char* key, int dflt) {
    Preferences p; p.begin(NS, true);
    int v = p.getInt(key, dflt);
    p.end();
    return v;
}
bool readBool(const char* key, bool dflt) {
    Preferences p; p.begin(NS, true);
    bool v = p.getBool(key, dflt);
    p.end();
    return v;
}
String readString(const char* key, const char* dflt) {
    Preferences p; p.begin(NS, true);
    String v = p.getString(key, dflt ? dflt : "");
    p.end();
    return v;
}
bool writeInt(const char* key, int v)  {
    Preferences p; if (!p.begin(NS, false)) return false;
    bool ok = p.putInt(key, v) > 0; p.end(); return ok;
}
bool writeBool(const char* key, bool v) {
    Preferences p; if (!p.begin(NS, false)) return false;
    bool ok = p.putBool(key, v); p.end(); return ok;
}
bool writeString(const char* key, const String& v) {
    Preferences p; if (!p.begin(NS, false)) return false;
    bool ok = p.putString(key, v) > 0; p.end(); return ok;
}

// Live-apply hook for fields whose effect must be visible immediately on
// patch (rather than waiting for the next read of the NVS value). Most
// schema fields are read at use-site (route_timeout in HTTP calls,
// log_to_sd in the logger, default_tier in the router) and naturally
// reflect the new value on the next call — no hook needed. The ones
// listed here own *driver state* that has to be pushed.
//
// Called from applyConfigJson() after a successful NVS write. The hook
// runs on the AsyncWebServer task (whichever core that lives on); the
// targeted writes (PWM duty for brightness, etc.) are atomic from any
// context.
void liveApply(const char* key) {
    if (strcmp(key, "brightness") == 0) {
        jarvis::hal::Display::setBrightness(jarvis::NVSConfig::getBrightness());
    } else if (strcmp(key, "tts_volume") == 0) {
        // M5.Speaker's mixer applies volume per output sample, so the
        // change takes effect on both currently-streaming audio and
        // future playback. AudioPlayer::setVolume clamps to [0, 100]
        // and maps to the speaker's 0–255 range internally.
        jarvis::hal::AudioPlayer::setVolume(jarvis::NVSConfig::getTtsVolume());
    }
    // Future: mic_gain → LLMModule audio.work parameter, wake_sens →
    // KWS threshold. Both want their own design pass — they involve
    // re-issuing setup commands to the Module-LLM over UART, which is
    // more invasive than a single register write.
}

}  // namespace

void buildConfigJson(JsonDocument& doc) {
    JsonArray fields = doc["fields"].to<JsonArray>();
    for (size_t i = 0; i < kSchemaCount; ++i) {
        const auto& f = kSchema[i];
        JsonObject o = fields.add<JsonObject>();
        o["key"]       = f.key;
        o["label"]     = f.label;
        o["category"]  = f.category;
        o["sensitive"] = f.sensitive;

        switch (f.type) {
            case FieldType::Bool:
                o["type"]    = "bool";
                o["value"]   = readBool(f.key, f.bdefault);
                o["default"] = f.bdefault;
                break;
            case FieldType::Int:
                o["type"]    = "int";
                o["value"]   = readInt(f.key, f.idefault);
                o["min"]     = f.imin;
                o["max"]     = f.imax;
                o["default"] = f.idefault;
                break;
            case FieldType::String: {
                o["type"] = "string";
                String v = readString(f.key, f.sdefault);
                if (f.sensitive) {
                    o["value"] = v.length() ? "********" : "";
                } else {
                    o["value"] = v;
                }
                break;
            }
            case FieldType::Enum: {
                o["type"]  = "enum";
                o["value"] = readString(f.key, f.sdefault);
                JsonArray opts = o["options"].to<JsonArray>();
                for (size_t j = 0; j < f.optionCount; ++j) {
                    JsonObject opt = opts.add<JsonObject>();
                    opt["value"] = f.options[j].value;
                    opt["label"] = f.options[j].label;
                }
                break;
            }
        }
    }
}

int applyConfigJson(const JsonDocument& patch) {
    int updated = 0;
    for (size_t i = 0; i < kSchemaCount; ++i) {
        const auto& f = kSchema[i];
        // ArduinoJson v7: presence check via containsKey() pattern.
        if (!patch[f.key].is<JsonVariantConst>()) continue;

        switch (f.type) {
            case FieldType::Bool: {
                bool v = patch[f.key].as<bool>();
                if (!writeBool(f.key, v)) return -1;
                break;
            }
            case FieldType::Int: {
                int v = patch[f.key].as<int>();
                if (v < f.imin || v > f.imax) return -1;
                if (!writeInt(f.key, v)) return -1;
                break;
            }
            case FieldType::String: {
                String v = patch[f.key].as<String>();
                // Sensitive: skip if user kept the masked placeholder.
                if (f.sensitive && v == "********") continue;
                if (!writeString(f.key, v)) return -1;
                break;
            }
            case FieldType::Enum: {
                String v = patch[f.key].as<String>();
                bool valid = false;
                for (size_t j = 0; j < f.optionCount; ++j) {
                    if (v == f.options[j].value) { valid = true; break; }
                }
                if (!valid) return -1;
                if (!writeString(f.key, v)) return -1;
                break;
            }
        }
        liveApply(f.key);   // driver state push for fields that need it
        ++updated;
    }
    return updated;
}

}  // namespace jarvis::app

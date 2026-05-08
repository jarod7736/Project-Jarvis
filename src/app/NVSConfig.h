#pragma once

// NVSConfig — thin wrapper over Preferences for the "jarvis" NVS namespace.
//
// Per CLAUDE.md: credentials live in NVS, namespace "jarvis", keys ≤15 chars.
// Never hardcode WiFi/HA/OpenClaw secrets. First-run provisioning is via USB
// Serial JSON when `wifi0_ssid` is empty.
//
// This is the seed of `app/NVSConfig` that PLAN.md Phase 3 prescribes — only
// the WiFi-slot-0 keys are wired up here; HA, OpenClaw, hotspot, and FW URL
// keys will land in their respective phases.

#include <Arduino.h>

namespace jarvis {

class NVSConfig {
public:
    static String getWiFi0SSID();
    static String getWiFi0Pass();
    static bool   setWiFi0(const String& ssid, const String& pass);

    // Home Assistant. Token never re-enters Serial output (security parity
    // with the WiFi password). `ha_host` falls back to config::kHaHostDefault
    // when unset.
    static String getHaToken();
    static bool   setHaToken(const String& token);
    static String getHaHost();
    static bool   setHaHost(const String& host);

    // OpenClaw (Phase 6). Same security pattern as HA; `oc_host` falls back
    // to config::kOpenclawHostDefault.
    static String getOcKey();
    static bool   setOcKey(const String& key);
    static String getOcHost();
    static bool   setOcHost(const String& host);

    // Cloud TTS (Phase 7). `tts_provider` is one of "openai", "eleven", or
    // "melotts" (force-local). Empty/unset falls back to
    // config::kTtsProviderDefault — currently "melotts" so existing
    // devices behave unchanged. `tts_voice_id` and `tts_model` are
    // provider-specific strings; the routing layer interprets them. The
    // bearer token is held in `tts_api_key` and is never echoed back to
    // the serial console (same security parity as ha_token / oc_key).
    static String getTtsProvider();
    static bool   setTtsProvider(const String& provider);
    static String getTtsVoiceId();
    static bool   setTtsVoiceId(const String& voiceId);
    static String getTtsApiKey();
    static bool   setTtsApiKey(const String& key);
    static String getTtsModel();
    static bool   setTtsModel(const String& model);

    // Optional prosody hint for OpenAI's `gpt-4o-mini-tts` model. Plain
    // English description of cadence / emphasis / tone that the model
    // applies on top of the chosen voice. Ignored by `tts-1` and
    // `tts-1-hd` (they reject the `instructions` field), so TtsClient
    // only emits it when `tts_model` starts with "gpt-4o". Empty/unset
    // → field omitted from the request body.
    static String getTtsInstructions();
    static bool   setTtsInstructions(const String& instr);

    // OTA (Phase 7). `fw_url` is the firmware bin URL pulled by the
    // "update firmware" voice intent (HTTPUpdate). `ota_pass` is the
    // ArduinoOTA password — when unset, the LAN OTA service stays
    // disabled (safe default; CLAUDE.md credentials-in-NVS rule).
    // Both keys are ≤15 chars per the namespace constraint.
    static String getFwUrl();
    static bool   setFwUrl(const String& url);
    static String getOtaPass();
    static bool   setOtaPass(const String& pass);

    // MQTT (Phase 7). `mqtt_host` is the broker host or IP (HA Mosquitto
    // add-on default). When empty, MqttClient stays disabled. `mqtt_user`
    // / `mqtt_pass` are the broker credentials — secrets, never echoed.
    static String getMqttHost();
    static bool   setMqttHost(const String& host);
    static String getMqttUser();
    static bool   setMqttUser(const String& user);
    static String getMqttPass();
    static bool   setMqttPass(const String& pass);

    // Blocks reading USB Serial up to `timeoutMs` for a single JSON line.
    // The provisioning JSON is a bag of optional fields:
    //   {"ssid":"...", "pass":"...", "ha_token":"...", "ha_host":"..."}
    // Each present key is written to NVS; absent keys are left alone. The
    // WiFi-only flow that enters from begin() requires `ssid`+`pass` —
    // see `provisionWiFiFromSerial()` below for that variant. This generic
    // entry point is what Phase 4+ provisioning calls.
    static bool provisionFromSerial(uint32_t timeoutMs = 180000);

    // Phase 3 entry point — same wire format, but rejects input that's
    // missing `ssid`. Kept for the WiFi-cred-required boot path.
    static bool provisionWiFiFromSerial(uint32_t timeoutMs = 180000);

    // ── Tunables surfaced through the captive-portal UI ────────────────
    // These overlap with config.h compile-time constants but live in NVS
    // so an end user can adjust them at runtime via the web UI without a
    // re-flash. Code that consults them should fall back to the config.h
    // default when the NVS value is absent (use the *Default helpers).
    //
    // All keys ≤15 chars per the namespace constraint.

    // TTS playback volume, 0–100. Mapped to M5.Speaker setVolume() at
    // begin() time and on change. Default 70.
    static int  getTtsVolume();
    static bool setTtsVolume(int pct);

    // Wake-word sensitivity, 1 (least sensitive) – 10 (most). Translates
    // to KWS module threshold; tuned values are device-specific. Default 5.
    static int  getWakeSens();
    static bool setWakeSens(int v);

    // Mic input gain, 0–100. Default 50. Mapped to M5Module-LLM
    // audio.work parameters at boot.
    static int  getMicGain();
    static bool setMicGain(int pct);

    // Default routing tier — one of "auto", "local", "cloud", "qwen".
    // "auto" falls back to the connectivity-tier-driven router; the
    // others force a specific backend. Default "auto".
    static String getDefaultTier();
    static bool   setDefaultTier(const String& tier);

    // HTTP timeout (ms) for the routing layer's outbound calls, 500–10000.
    // Default 3000.
    static int  getRouteTimeout();
    static bool setRouteTimeout(int ms);

    // Whether to mirror voice-loop transcripts to the SD card via
    // hal/SdLogger. Default true.
    static bool getLogToSd();
    static bool setLogToSd(bool on);

    // Display backlight brightness, 10–255. Default 180. Applied to
    // M5.Display.setBrightness() at boot and on change.
    static int  getBrightness();
    static bool setBrightness(int v);

    // Auto-sleep timeout (seconds) — 0 disables. Default 60. Used by
    // ModeManager to dim/blank the display after inactivity.
    static int  getSleepSecs();
    static bool setSleepSecs(int s);
};

}  // namespace jarvis

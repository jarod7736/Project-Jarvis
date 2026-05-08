#pragma once

// Project Jarvis — compile-time constants.
// Group by subsystem. When something gets added here that's only used in one
// .cpp, prefer a file-local constant in that .cpp. Symbols here are the
// cross-cutting ones.

namespace jarvis::config {

// ── Hardware ──────────────────────────────────────────────────────────────
// CoreS3 SD card on its own SPIClass instance (CLAUDE.md SPI-conflict rule).
// Not used in Phase 2 yet — kept here so Phase 7 SD logger can reference it.
constexpr int kSdCs   = 4;
constexpr int kSdSck  = 36;
constexpr int kSdMiso = 35;
constexpr int kSdMosi = 37;

// M5Bus UART to LLM Module: 115200 8N1 on Port C. Pins resolved at runtime
// via M5.getPin(port_c_*) — they differ across CoreS3 revisions, so don't
// hardcode here.

// ── LLM Module firmware pin ───────────────────────────────────────────────
// Re-test the UART JSON path after every StackFlow FW bump (CLAUDE.md). The
// API drifts: e.g. v1.3 deprecates ApiAudio::setup, claims audio is "auto-
// configured internally" — but Phase 1 retro shows mic capture doesn't start
// without an explicit audio.work send.
constexpr const char* kStackflowFwPin = "v1.6";

// ── Voice-loop timeouts ───────────────────────────────────────────────────
// LISTEN: idle return after silence. Matched by ASR rule3 (max recognition
// timeout) below — keep them in sync.
constexpr uint32_t kListenTimeoutMs = 10000;

// melotts setup is empirically slow on this firmware (~16s lexicon load) but
// the lib's hardcoded ceiling is 15s. We bypass via raw sendCmd; this is the
// budget for the manual response wait.
constexpr uint32_t kMelottsSetupMs = 60000;

// SPEAKING duration estimate for the Phase 2 echo stub. Real TTS-done
// detection lands in Phase 5/6 when LLM is in the chain. Tuned for English at
// roughly 15 chars/sec + 600ms playback tail.
constexpr uint32_t kSpeakBaseMs       = 600;
constexpr uint32_t kSpeakMsPerChar    = 70;

// ── ASR pacing rules (seconds, per Module-LLM API doc) ────────────────────
constexpr float kAsrRule1 = 2.4f;   // unrecognized-content timeout from wake
constexpr float kAsrRule2 = 1.2f;   // max interval between recognized chunks
constexpr float kAsrRule3 = 10.0f;  // total recognition deadline (=kListenTimeoutMs)

// ── Wake word ─────────────────────────────────────────────────────────────
// StackFlow's KWS unit accepts an arbitrary all-caps English wake word
// at runtime via the `kws` field in kws.setup — no custom-trained model
// required. The underlying sherpa-onnx Zipformer (gigaspeech-3.3M)
// generalises to any in-vocab word. Constraints:
//   - Must be ALL CAPS for English (lowercase rejected by the daemon)
//   - No mixing English + Chinese in the same string
//   - kws.setup takes ~9 s on cold boot; existing setup timeout covers it
//
// Reverted to "HELLO" temporarily — "JARVIS" was set on hardware and
// failed to wake (silent kws.setup failure, possibly missing model
// package or library forcing a stale model name). Debug later; HELLO
// is the known-working baseline that Phase 1 validated.
constexpr const char* kWakeWord = "HELLO";

// ── Connectivity tier probes (PLAN.md Phase 3) ────────────────────────────
// Defaults from CLAUDE.md "External endpoints". Phase 4 (HA) and Phase 6
// (OpenClaw) add NVS overrides at keys `ha_host` and `oc_host`.
constexpr const char* kHaHostDefault       = "pczxegrio1uswrn1pi0c2cpnfdjomwkx.ui.nabu.casa";
constexpr uint16_t    kHaPortDefault       = 443;   // Nabu Casa cloud HTTPS
// OpenClaw default points at LM Studio on jarod-desktop's LAN IP. The
// canonical Tailscale URL (https://lobsterboy.tail1c66ec.ts.net) doesn't
// resolve from the CoreS3 — the device isn't on Tailscale and the router's
// DNS doesn't know *.ts.net. To use Tailscale, configure a subnet router.
constexpr const char* kOpenclawHostDefault = "http://192.168.1.108:1234";
// Tier probe needs bare host:port — parsed out of kOpenclawHostDefault.
// Keep this in sync if the URL changes; URL parsing in C++ on Arduino isn't
// worth the cost for a one-line constant.
constexpr const char* kOpenclawProbeHost = "192.168.1.108";
constexpr uint16_t    kOpenclawPortDefault = 1234;

// ── OpenClaw model routing (PLAN.md Phase 6) ──────────────────────────────
// User's OpenClaw runs LM Studio on jarod-desktop. Local model is
// google/gemma-4-e4b. Claude model is TBD (not currently loaded — will
// fail until configured); kept here so the routing structure is in place.
constexpr const char* kOcLocalModel  = "google/gemma-4-e4b";
constexpr const char* kOcClaudeModel = "claude-sonnet-4-6";  // TBD

// HTTP timeout for the OpenClaw call. PLAN.md says 10s; the Tailscale
// network adds a small margin to that.
constexpr uint32_t kOcHttpTimeoutMs = 12000;
// Cap responses to a length that fits in TTS without dragging on. Lowered
// from 150 → 80 tokens because gemma-4-e4b spends long contexts narrating
// its own reasoning ("The user is asking... I should..."). Forcing it
// short crowds out the inner monologue and leaves more room for an
// actual answer per token spent.
constexpr int      kOcMaxTokens     = 80;
constexpr size_t   kOcMaxReplyChars = 400;

// ── Cloud TTS routing (PLAN.md Phase 7) ───────────────────────────────────
// Defaults baked into config so a fresh device behaves identically to the
// pre-Phase-7 build until the user provisions tts_api_key. The router in
// LLMModule::speak() checks `tts_provider == "melotts"` || `tts_api_key`
// is empty and falls through to the on-device melotts path in that case.
constexpr const char* kTtsProviderDefault = "melotts";
// Walken-adjacent presets per provider — see PLAN.md Phase 7 legal note
// for why we don't ship a celebrity-clone voice ID. `onyx` is OpenAI's
// deepest male voice and the closest stock match for the gravelly older-
// male register without impersonating a specific person.
constexpr const char* kTtsVoiceIdDefault = "onyx";
constexpr const char* kTtsModelDefault   = "tts-1";

// Cloud TTS endpoints. OpenAI is the cheapest path; ElevenLabs is the
// quality+voice-variety path. Both speak HTTPS; both return a streamable
// audio body. The router picks at request time based on tts_provider —
// see net/TtsClient.cpp.
constexpr const char* kTtsOpenAIHost     = "api.openai.com";
constexpr const char* kTtsOpenAIPath     = "/v1/audio/speech";
constexpr const char* kTtsElevenHost     = "api.elevenlabs.io";
// Path is voice-suffixed at request time: "/v1/text-to-speech/<voice_id>"
constexpr const char* kTtsElevenPathBase = "/v1/text-to-speech/";

// 15 s total HTTP budget. Phase 7 streaming impl can chunk-download into
// a ring buffer so playback starts before the full payload arrives, but
// the buffered MVP just downloads the whole MP3 then plays.
constexpr uint32_t kTtsHttpTimeoutMs = 15000;
// Cap downloaded MP3 size to avoid runaway. ~30 KB ≈ 10 s of speech at
// 24 kbps; 120 KB gives ~40 s headroom for verbose Claude replies.
constexpr size_t   kTtsMaxMp3Bytes   = 120 * 1024;

// ── OTA (PLAN.md Phase 7) ─────────────────────────────────────────────────
// LAN-side ArduinoOTA mDNS hostname — appears as "jarvis.local" in the
// Arduino IDE / PlatformIO upload-port picker. Pinned here (not NVS) so
// every device on the LAN is discoverable by the same name regardless of
// flash state. Port 3232 is the espressif32 default; explicit so it shows
// up in netstat if anything probes the bring-up.
constexpr const char* kOtaHostname = "jarvis";
constexpr uint16_t    kOtaPort     = 3232;

// HTTPUpdate budget for remote OTA pulls. Generous — flashing 1 MB over a
// slow LAN can take 30–60 s. Watchdog is fed via the progress callback in
// OtaService, so the only real ceiling is the HTTP TCP timeout the lib
// imposes on each chunk.
constexpr uint32_t kOtaHttpTimeoutMs = 60000;

// User-facing strings for the update_fw intent.
constexpr const char* kOtaSpeakStart  = "Updating firmware now. Back in a minute.";
constexpr const char* kOtaSpeakNoUrl  = "Firmware URL is not configured.";
constexpr const char* kOtaSpeakFailed = "Firmware update failed.";

// ── MQTT (PLAN.md Phase 7) ────────────────────────────────────────────────
// HA Mosquitto add-on default: port 1883, authenticated. Broker host
// lives in NVS as `mqtt_host` (parity with `ha_host` / `oc_host`).
// Empty host → service disabled, no reconnect attempts.
constexpr uint16_t    kMqttPort         = 1883;
constexpr const char* kMqttClientIdBase = "jarvis";   // suffixed with MAC tail
constexpr const char* kMqttTopicState   = "jarvis/state";
constexpr const char* kMqttTopicCommand = "jarvis/command";
// Last-Will payload published by the broker if the device drops without
// a clean disconnect — lets HA automations detect a stuck/crashed Jarvis.
constexpr const char* kMqttLwtPayload   = "OFFLINE";
// Reconnect cadence per PLAN.md:691 — 30 s, non-blocking. Tracked
// against millis() in MqttClient::tick(); never sleeps the loop.
constexpr uint32_t    kMqttReconnectMs  = 30000;
// PubSubClient's default keepalive is 15s; bump to 60 so we don't churn
// on every routine probe.
constexpr uint16_t    kMqttKeepaliveSec = 60;

// ── Hardware watchdog (PLAN.md Phase 7) ───────────────────────────────────
// 30 s timeout. Generous on purpose: a normal voice cycle is 1–6 s end to
// end, but cloud TTS+LLM can stretch to 10+ s on a slow link. The watchdog
// is the safety net for genuine hangs (UART deadlock, HTTPClient leak with
// the lib's own timeout disabled), not a per-call deadline. loop()'s
// watchdog reset fires every iteration so anything that returns control
// to loop() — even slowly — keeps us fed.
constexpr uint32_t kWatchdogTimeoutSec = 30;

// Per-probe timeouts. Tier re-check runs at most this often from
// loop()-driven polling — keep them tight so a single tier-check pass stays
// under ~6s end-to-end and the FSM doesn't stutter audibly.
constexpr uint32_t kHaProbeMs       = 2000;
constexpr uint32_t kOpenclawProbeMs = 3000;
constexpr uint32_t kTierRecheckMs   = 30000;

// ── Display regions (CoreS3 320×240, see hal/Display.cpp) ─────────────────
// Region geometry is duplicated as anonymous-namespace constants in
// hal/Display.cpp. Display owns the rendering; this header just exposes the
// names for any future state inspection. Don't divergence-test these.

// ── Error strings (PLAN.md "Error Response Taxonomy") ─────────────────────
// User-facing TTS strings for failure paths. Phase 4+ wires these into
// dispatch handlers; defining now keeps config.h from being touched again.
constexpr const char* kErrNoNetwork      = "I'm offline. Ask me something simple.";
constexpr const char* kErrHaUnreachable  = "I couldn't reach home assistant.";
constexpr const char* kErrLlmTimeout     = "That's taking too long. Try again.";
constexpr const char* kErrIntentParse    = "I wasn't sure what you meant. Could you rephrase?";
constexpr const char* kErrModuleOffline  = "My A I module is restarting.";

}  // namespace jarvis::config

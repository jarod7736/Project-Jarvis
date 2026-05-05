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
// Phase 2.5 retrains the KWS asset for "JARVIS" and flips this constant.
// Until then we use the bundled HELLO model that Phase 1 already validated.
constexpr const char* kWakeWord = "HELLO";

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

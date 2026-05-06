# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

**Pre-code, pre-hardware.** The repo currently contains only `README.md` and `PLAN.md`. There is no `Jarvis.ino`, no `src/`, no build system, no tests. Hardware (M5Stack CoreS3 + LLM Module) is en route. Do not invent build/lint/test commands — none exist yet. When code starts to land, it will be Arduino C++ targeting ESP32-S3 via the Arduino IDE or PlatformIO with the M5Stack board package; standard `npm`/`pytest`/`make` workflows do not apply.

## Where to read first

`PLAN.md` is the source of truth for the firmware folder layout, library dependencies, FSM, NVS schema, intent prompt, phase-by-phase tasks, memory budget, error taxonomy, and known pitfalls. `README.md` is the high-level pitch. When asked to implement anything, start by re-reading the relevant phase section in `PLAN.md` — phases are gated by validation criteria and have a strict dependency order (1 → 2 → 3 → 4 → 5 → 6 → 7; phases 2 and 3 may go in parallel after 1).

## Architectural invariants (do not violate)

These cut across multiple sections of the plan and are easy to miss from any single file:

- **The Qwen 0.5B on the LLM Module is a router and formatter, not the primary intelligence.** Anything beyond simple commands escalates to OpenClaw or Claude. Do not try to make Qwen answer complex queries — it will hallucinate.
- **Single-threaded FSM, no RTOS.** `loop()` calls `tickStateMachine()` once per iteration. KWS/ASR/TTS callbacks only *set flags* (`g_asrReady`, transcript buffer, etc.) — they must never block, call TTS, or transition state directly.
- **TTS and ASR cannot run simultaneously.** `SPEAKING → IDLE` is driven only by the TTS-done callback, never by a timer.
- **HTTP calls are blocking.** The display must show "Thinking..." *before* the call starts, because `loop()` stalls during `http.GET()`/`http.POST()`. Streaming is explicitly out of scope until Phase 7.
- **`http.end()` must fire on every exit path** in `LLMClient::query()` (timeout, parse error, early return). `WiFiClientSecure` + `HTTPClient` leaks otherwise.
- **PSRAM rule.** Any buffer >512 bytes goes in PSRAM (`ps_malloc`, `DynamicJsonDocument`). Small request bodies stay on the stack as `char[256]`. The ESP32-S3 has 8MB PSRAM — enable it in board settings.
- **Connectivity is tiered, not binary.** `getConnectivityTier()` returns one of `LAN | TAILSCALE | HOTSPOT_ONLY | OFFLINE` and code must branch on it. Tier 4 (OFFLINE) bypasses the intent router entirely and goes straight to local Qwen with a *plain-text* offline prompt (no JSON).
- **Intent JSON from Qwen is dirty.** It frequently arrives wrapped in ` ```json ... ``` ` backtick fences. `IntentRouter` must call `stripJsonMarkdown()` before `deserializeJson()`. On parse failure, log the raw output and fall back to `local_llm` — never silently drop to IDLE without TTS feedback.
- **Credentials live in NVS, namespace `"jarvis"`, keys ≤15 chars.** Never hardcode WiFi/HA/OpenClaw secrets. First-run provisioning is via USB Serial JSON when `wifi0_ssid` is empty. See the NVS Schema table in `PLAN.md` for the exact key list.
- **StackFlow firmware version on the LLM Module drifts.** Pin the version (returned by `module_llm.sys.ping()`) as a comment in `config.h` and re-test after every module FW update — the UART JSON API has shifted across versions.
- **Per-AP WiFi timeout.** Use `WiFiMulti.run(500)` for fast failover; the default is 15–30s.
- **CoreS3 SPI conflict.** SD card and display share SPI on CoreS3. Initialize SD on its own `SPIClass` instance with explicit pin assignment (CS=4, SCK=36, MISO=35, MOSI=37). M5Bus UART to the LLM Module is `Serial2` at 115200 8N1 — confirm pins against `M5Module-LLM` source before assuming GPIO16/17.

## External endpoints

- **OpenClaw** (Tailscale): `https://lobsterboy.tail1c66ec.ts.net` — OpenAI-compatible `POST /v1/chat/completions`. Used for both local-LLM and Claude routing (model selected via the `model` field; constants in `config.h` will be `OC_LOCAL_MODEL` and `OC_CLAUDE_MODEL`).
- **Home Assistant** (Nabu Casa): `pczxegrio1uswrn1pi0c2cpnfdjomwkx.ui.nabu.casa` — REST API, bearer-token auth from NVS (`ha_token`). Use `WiFiClientSecure` with `setInsecure()` for now; cert pinning is a deferred TODO.

## Reference documentation

Local copies of canonical hardware/API references live under `docs/reference/`. Prefer these over web searches when answering questions about the LLM Module or the AX630C SoC — they're version-pinned and won't drift. See `docs/reference/README.md` for the full index.

Highest-leverage files for this project:
- `docs/reference/LLM_Module_API_v1.0.0_EN.pdf` — official StackFlow UART JSON API spec (sys/audio/kws/asr/llm/tts units, packet format, error codes, Voice Assistant chain). Per this spec the action to start a unit is `<unit>.work`, not `<unit>.cap` — contradicts the Phase 1 retro's hypothesis and should inform voice-loop wiring. Spec is v1.0.0 (2024-10-24); device firmware is `v1.3` per Phase 1 validation, so a few field names have shifted (`max_token_len` vs `max_length`).
- `docs/reference/m5stack/en/stackflow/module_llm/arduino_api.md` — Arduino library API reference. First place to look for the C++ surface of `module_llm.audio/kws/asr/llm/tts/sys`.
- `docs/reference/m5stack/axera_docs/05 - AX AUDIO API Document.pdf` — the layer below StackFlow's `audio.*` actions. Useful when StackFlow's audio behavior diverges from the spec (as in the Phase 1 retro).
- `docs/reference/m5stack/Sch_M5_Module-LLM.pdf` — Module schematic (M5Bus pinout, power tree).

## Adding new voice commands

In Phase 4 (pre-Qwen), commands are a static `CommandEntry[]` table in `app/CommandHandler.cpp` matched via `String::indexOf()` — intentionally brittle, gets replaced in Phase 5. From Phase 5 onward, new intents are added by (a) extending the few-shot examples in `prompts/intent_prompt.h` and (b) adding a dispatch branch in `IntentRouter`. Do not add intents without updating both.

## Error responses

All user-facing failures route through the `ERR_*` constants in `config.h` (see "Error Response Taxonomy" in `PLAN.md`). The pattern is always: `g_state = ERROR` → `display.setStatus(ERROR)` → `llmModule.speak(ERR_*)` → transition to `IDLE`. Never return to IDLE silently on an error path.

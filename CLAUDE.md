# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Session start

Before answering anything substantive, run `agentos --json --project project-jarvis project show` and load the result into your understanding of this project. After every architectural decision, run `agentos --project project-jarvis decision add ...`. agentos is the authoritative status board; this file is for invariants that don't change between phases.

## Project state

**Phases 1–8 all shipped. Phase 7 + Phase 8 are "closed for hardware-validation purposes"** per `plans/phase7-validation.md` and `plans/phase8-2nd-brain-validation.md` — see those files for which gates passed vs. were deferred, with rationale. Current active work is **post-validation Sprint 1/2** features:

- `tools/notifier/` — FastAPI service on lobsterboy:8081 with a 3-tier priority router (high/medium/low). High publishes to MQTT `jarvis/speak` and Pushover; medium queues on disk and drains on the next IDLE; low logs only.
- `tools/morning-brief/` — scheduled 08:00 brief driven by `oc-personal` against `brain_list_projects` + `gcal_list_events`. Biases toward stale projects ("stalled projects can't hide").
- `tools/google-mcp/` — Gmail + Calendar MCP, joined to the brain-MCP via a multi-MCP runner under `tools/oc-personal-runner/`.
- `tools/brain-mcp/` — exposes 6 tools to the agent (`brain_search`, `brain_capture`, `brain_lint`, `brain_list_projects`, `brain_set_next_action`, `brain_ingest_status`). The runtime catalog is **12 tools** total (6 brain + 6 google); `brain_ingest` is scheduler-driven, not on the agent surface.

The repo is a PlatformIO project (`platformio.ini` + `src/`). It builds Arduino C++ for ESP32-S3 (M5Stack CoreS3). Build with `pio run` / `pio run -t upload` / `pio run -t uploadfs`. Local LLM backend defaults to **Ollama** on `192.168.1.108:11434` (was LM Studio; provider-agnostic proxy from PR #49). No `npm` / `pytest` / `make` workflow.

## Where to read first

In order:
1. `agentos --json --project project-jarvis project show` — current focus, goals, decisions, open questions.
2. `git log --oneline -20` — what shipped recently (Sprint 1/2 features have been landing fast, README/CLAUDE.md lag).
3. `README.md` — per-phase checkboxes, intelligence tiers, backends.
4. `plans/phase7-validation.md` + `plans/phase8-2nd-brain-validation.md` — what was actually exercised on hardware.
5. `PLAN.md` — historical design doc + retros. Section ordering: phase-by-phase implementation notes, NVS schema, error taxonomy, memory budget. Treat the per-phase text as historical; retros at the bottom of each section have what was actually built.

Phase dependency order (historical): 1 → 2 → 3 → 4 → 5 → 6 → 7; phases 2 and 3 ran in parallel; Phase 8 sits independent of 7.

## Architectural invariants (do not violate)

These cut across multiple sections of the plan and are easy to miss from any single file:

- **The Qwen 0.5B on the LLM Module is a best-effort hint, not a classifier.** The deterministic keyword classifier on the CoreS3 is the primary intent route (Phase 5 retro: `qwen2.5-0.5b-prefill-20e` doesn't reliably follow instruction prompts). Qwen runs as a 4-second optional pre-step. Anything beyond simple commands escalates to OpenClaw or Claude.
- **Single-threaded FSM, no RTOS.** `loop()` calls `tickStateMachine()` once per iteration. KWS/ASR/TTS callbacks only *set flags* — they must never block, call TTS, or transition state directly.
- **TTS and ASR cannot run simultaneously.** `SPEAKING → IDLE` is driven only by the TTS-done callback, never by a timer. With cloud TTS shipped, `LLMModule::speak()` is a router: cloud (OpenAI/ElevenLabs via `net/TtsClient`) → `hal/AudioPlayer`; on timeout, missing cloud config, or OFFLINE tier, it falls back to melotts on the LLM Module. Single call site in `state_machine.cpp`; `onPlayDone` fires the FSM transition either way.
- **Proactive pushes bypass the intent router.** MQTT `jarvis/speak` is consumed by `MqttClient` into a single-slot queue distinct from `jarvis/command`; from IDLE the FSM drains directly to `enterSpeaking()` with no Qwen / no LLM call. SD log marks these with `transcript="<push>"` for auditability.
- **HTTP calls are blocking.** The display must show "Thinking..." *before* the call starts, because `loop()` stalls during `http.GET()`/`http.POST()`.
- **`http.end()` must fire on every exit path** in `LLMClient::query()` / `AnthropicClient` / `TtsClient` / `HAClient` (timeout, parse error, early return). `WiFiClientSecure` + `HTTPClient` leaks otherwise.
- **PSRAM rule.** Any buffer >512 bytes goes in PSRAM (`ps_malloc`, `DynamicJsonDocument`). Small request bodies stay on the stack as `char[256]`. The CoreS3 has **Quad-SPI PSRAM, not Octal** (`flash_mode=qio`, `psram_type=qio`, set in `platformio.ini` per PR #54). The MP3 cap for cloud TTS is **256 KB** in PSRAM (raised from 64 KB after the PSRAM detection was fixed, PR #55).
- **Connectivity is tiered, not binary.** `getConnectivityTier()` returns one of `LAN | TAILSCALE | HOTSPOT_ONLY | OFFLINE`. Code branches on it. OFFLINE bypasses the intent router and goes straight to local Qwen with a *plain-text* offline prompt (no JSON). `personal_query` / `journal_note` / calendar / email intents require LAN or TAILSCALE — on HOTSPOT_ONLY / OFFLINE the firmware short-circuits to `kErrPersonalOffline` rather than burning HTTP timeout budget. The tier probe follows the NVS `oc_host` (PR #48) — don't pin it to a hardcoded LM Studio address.
- **Intent JSON from Qwen is dirty.** It frequently arrives wrapped in ` ```json ... ``` ` backtick fences. `IntentRouter` must call `stripJsonMarkdown()` before `deserializeJson()`. On parse failure, log the raw output and fall back to `local_llm` — never silently drop to IDLE without TTS feedback.
- **Credentials live in NVS, namespace `"jarvis"`, keys ≤15 chars.** Never hardcode WiFi / HA / OpenClaw / Anthropic / TTS / OTA secrets. First-run provisioning is `tools/provision-wifi.py`. See the NVS Schema table in `PLAN.md` for the exact key list; `tts_model` was added in PR #50 and is exposed in the captive portal.
- **StackFlow firmware version on the LLM Module drifts.** Currently pinned at `v1.3` (per Phase 1 validation). Re-test after every module FW update — the UART JSON API has shifted across versions.
- **Per-AP WiFi timeout.** Use `WiFiMulti.run(500)` for fast failover; the default is 15–30s. Slot priority is true slot-order (PR #20), not a per-attempt race. After NTP sync, use `configTzTime` so the configured TZ survives (PR-from-`worktree-phase7-validation`).
- **CoreS3 SPI conflict.** SD card and display share SPI on CoreS3. Initialize SD on its own `SPIClass` instance with explicit pin assignment (CS=4, SCK=36, MISO=35, MOSI=37). M5Bus UART to the LLM Module uses Port C: `M5.getPin(port_c_rxd/txd)` — RX=18, TX=17, **not** GPIO16/17.
- **OTA must survive every build.** `ArduinoOTA` is initialized after WiFi in `OtaService` and requires `ota_pass` from NVS. If you remove or forget to call it in a future refactor, the next USB flash silently kills the OTA path — re-flashing requires physical USB. Treat OTA as a load-bearing dependency.

## External endpoints

- **OpenClaw / personal agent** (LAN-first, Tailscale path also available): `http://192.168.1.178:8080` on the LAN, `https://lobsterboy.tail1c66ec.ts.net` via Tailscale — OpenAI-compatible `POST /v1/chat/completions`, served by `tools/oc-personal-runner/`. `model="oc-personal"` runs a Claude + multi-MCP agent loop (12 tools: 6 brain + 6 google) on lobsterboy; any other model name is proxied to the configured local backend. The device hits the bare LAN port; the Tailscale path is for off-LAN access.
- **Local LLM backend**: Ollama at `http://192.168.1.108:11434` by default (PR #49 made the proxy provider-agnostic). NVS `oc_host` overrides; `LM_STUDIO_TOKEN` is forwarded as `Bearer` on passthrough (PR #37) so a token-protected LM Studio install still works.
- **Anthropic (direct)**: `api.anthropic.com` — used by `AnthropicClient` for the `claude` intent on HOTSPOT_ONLY (PR #19). Bearer token in NVS `anth_key`.
- **Home Assistant** (Nabu Casa): `pczxegrio1uswrn1pi0c2cpnfdjomwkx.ui.nabu.casa` — REST API, bearer-token auth from NVS (`ha_token`). `WiFiClientSecure` with `setInsecure()`; cert pinning is a deferred TODO. MQTT broker on HA (`mqtt_host` in NVS); topics `jarvis/state`, `jarvis/command`, `jarvis/speak`.
- **Cloud TTS**: OpenAI `/v1/audio/speech` or ElevenLabs `/v1/text-to-speech/<voice_id>` per NVS `tts_provider`. Per-source provider routing exists (PR #43): proactive pushes can use a different voice than reactive replies. melotts on the LLM Module is the fallback.
- **Notifier**: lobsterboy:8081 `POST /notify` with `tier=high|medium|low`. Companion CLI: `scripts/notifier/send.sh`.

## Adding new voice commands / intents

New intents require updating **both** sides:
1. The keyword table in `src/app/CommandHandler.cpp` (primary route — deterministic, dispatched by `IntentRouter`).
2. The few-shot examples in `src/prompts/intent_prompt.h` (Qwen's best-effort hint path).

Skipping either side breaks one of the two routes silently. The keyword table is the source of truth for what gets dispatched; the prompt only nudges Qwen toward consistent labels.

Calendar / email / project utterances route to `personal_query` (PR #36) so they all flow through the multi-MCP agent — don't add separate intents for them.

## Error responses

All user-facing failures route through the `kErr*` constants in `src/config.h` (see "Error Response Taxonomy" in `PLAN.md`). The pattern is always: `g_state = ERROR` → `display.setStatus(ERROR)` → `llmModule.speak(kErr*)` → transition to `IDLE`. Never return to IDLE silently on an error path.

## Deployment

Lobsterboy-side services (`brain-mcp`, `oc-personal-runner`, `google-mcp`, `morning-brief`, `notifier`) all use the same pattern: `tools/<service>/deploy.sh` + systemd unit templates with `__RUN_USER__` / `__PROJECT_ROOT__` placeholders (PR #32). Deploy from a Tailscale-reachable host; the script handles unit install, venv setup, and `systemctl daemon-reload`.

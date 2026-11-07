# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Session start

There is no status-board command. An earlier version of this file opened by
telling you to run `agentos --json --project project-jarvis project show`; that
binary is not installed on any current dev machine, and the board it fronted is
not reachable. Don't go looking for it.

Read `git log --oneline -20` first — Sprint work lands fast and the prose docs
lag it. Then the list under "Where to read first". Record architectural
decisions in the relevant `plans/*.md` doc, or as a retro at the end of the
matching `PLAN.md` section; that's where every prior decision actually lives.
This file is for invariants that don't change between phases.

## Project state

**Phases 1–8 all shipped. Phase 7 + Phase 8 are "closed for hardware-validation purposes"** per `plans/phase7-validation.md` and `plans/phase8-2nd-brain-validation.md` — see those files for which gates passed vs. were deferred, with rationale. Current active work is **post-validation Sprint 1/2** features:

- `tools/notifier/` — FastAPI service on lobsterboy:8081 with a 3-tier priority router (high/medium/low). High publishes to MQTT `jarvis/speak` and Pushover; medium queues on disk and drains on the next IDLE; low logs only.
- `tools/morning-brief/` — scheduled 08:00 brief driven by `oc-personal` against `brain_list_projects` + `gcal_list_events`. Biases toward stale projects ("stalled projects can't hide"). **Stopped 2026-08-30** — the agent response (~56s) exceeded the 60s timeout; needs a timeout bump or faster model before re-enabling.
- `tools/google-mcp/` — Gmail + Calendar MCP, joined to the brain-MCP via a multi-MCP runner under `tools/oc-personal-runner/`.
- `tools/brain-mcp/` — exposes 6 tools to the agent (`brain_search`, `brain_capture`, `brain_lint`, `brain_list_projects`, `brain_set_next_action`, `brain_ingest_status`). The runtime catalog is **12 tools** total (6 brain + 6 google); `brain_ingest` is scheduler-driven, not on the agent surface.

The repo is a PlatformIO project (`platformio.ini` + `src/`). It builds Arduino C++ for ESP32-S3 (M5Stack CoreS3). Build with `pio run` / `pio run -t upload` / `pio run -t uploadfs`. Local LLM backend defaults to **Lemonade** on `amd-halo:13305` (was Ollama on 192.168.1.108; provider-agnostic proxy from PR #49). The runner rewrites passthrough model names to `coder` (a Lemonade alias) via `OC_PROXY_FORCE_MODEL`. No `npm` / `pytest` / `make` workflow.

## Where to read first

In order:
1. `git log --oneline -20` — what shipped recently (Sprint 1/2 features have been landing fast, README/CLAUDE.md lag).
2. `AGENTS.md` — the executable facts: how to build, what isn't installed, which `deploy.sh` subcommands exist, and the fact that there is no test/lint/CI anywhere in this repo.
3. `README.md` — per-phase checkboxes, intelligence tiers, backends.
4. `plans/lemonade-llm-tts.md` + `plans/lemonade-runbook.md` — the current LLM/TTS backend and its operational state. This is the live frontier; read it before touching anything model- or TTS-related.
5. `plans/phase7-validation.md` + `plans/phase8-2nd-brain-validation.md` — what was actually exercised on hardware.
6. `PLAN.md` — historical design doc + retros. Section ordering: phase-by-phase implementation notes, NVS schema, error taxonomy, memory budget. Treat the per-phase text as historical; retros at the bottom of each section have what was actually built.

Phase dependency order (historical): 1 → 2 → 3 → 4 → 5 → 6 → 7; phases 2 and 3 ran in parallel; Phase 8 sits independent of 7.

## Architectural invariants (do not violate)

These cut across multiple sections of the plan and are easy to miss from any single file:

- **The Qwen 0.5B on the LLM Module is a best-effort hint, not a classifier.** The deterministic keyword classifier on the CoreS3 is the primary intent route (Phase 5 retro: `qwen2.5-0.5b-prefill-20e` doesn't reliably follow instruction prompts). Qwen runs as a 4-second optional pre-step. Anything beyond simple commands escalates to the oc-personal runner (Lemonade) or Claude.
- **Single-threaded FSM, no RTOS.** `loop()` calls `tickStateMachine()` once per iteration. KWS/ASR/TTS callbacks only *set flags* — they must never block, call TTS, or transition state directly.
- **TTS and ASR cannot run simultaneously.** `SPEAKING → IDLE` is driven only by the TTS-done callback, never by a timer. `LLMModule::speak()` is a router: it builds a short tier-aware candidate list (lemonade/kokoro on the LAN, OpenAI/ElevenLabs off-LAN) via `net/TtsClient` → `hal/AudioPlayer`, and falls back to melotts on the LLM Module when every candidate fails or the tier is OFFLINE. Single call site in `state_machine.cpp`; `onPlayDone` fires the FSM transition either way.
- **`TAILSCALE` tier means "on the LAN, internet down" — not "remote".** The tier probe targets `oc_host` (lobsterboy, a private address), so OC-reachable implies the home network. That makes `LAN` and `TAILSCALE` both LAN-reachable (amd-halo works, cloud may not) and `HOTSPOT_ONLY` the genuinely-remote tier. The CoreS3 cannot join a tailnet and lobsterboy's `tailscale serve` is tailnet-only, so `*.ts.net` names are never reachable from the device — use LAN IPs in device-side config.
- **Proactive pushes bypass the intent router.** MQTT `jarvis/speak` is consumed by `MqttClient` into a single-slot queue distinct from `jarvis/command`; from IDLE the FSM drains directly to `enterSpeaking()` with no Qwen / no LLM call. SD log marks these with `transcript="<push>"` for auditability.
- **HTTP calls are blocking.** The display must show "Thinking..." *before* the call starts, because `loop()` stalls during `http.GET()`/`http.POST()`.
- **`http.end()` must fire on every exit path** in `LLMClient::query()` / `AnthropicClient` / `TtsClient` / `HAClient` (timeout, parse error, early return). `WiFiClientSecure` + `HTTPClient` leaks otherwise.
- **PSRAM rule.** Any buffer >512 bytes goes in PSRAM (`ps_malloc`, `DynamicJsonDocument`). Small request bodies stay on the stack as `char[256]`. The CoreS3 has **Quad-SPI PSRAM, not Octal** (`flash_mode=qio`, `psram_type=qio`, set in `platformio.ini` per PR #54). The MP3 cap for cloud TTS is **256 KB** in PSRAM (raised from 64 KB after the PSRAM detection was fixed, PR #55).
- **Connectivity is tiered, not binary.** `getConnectivityTier()` returns one of `LAN | TAILSCALE | HOTSPOT_ONLY | OFFLINE`. Code branches on it. OFFLINE bypasses the intent router and goes straight to local Qwen with a *plain-text* offline prompt (no JSON). `personal_query` / `journal_note` / calendar / email intents require LAN or TAILSCALE — on HOTSPOT_ONLY / OFFLINE the firmware short-circuits to `kErrPersonalOffline` rather than burning HTTP timeout budget. The tier probe follows the NVS `oc_host` (PR #48) — it targets the **oc-personal runner on lobsterboy**, which is a different host from the LLM backend (Lemonade on amd-halo). Don't pin the probe to a hardcoded backend address: an earlier version pointed it at LM Studio, and repeating that mistake with `amd-halo:13305` would make the tier report on the wrong machine and break the `TAILSCALE`-tier reasoning above.
- **Intent JSON from Qwen is dirty.** It frequently arrives wrapped in ` ```json ... ``` ` backtick fences. `IntentRouter` must call `stripJsonMarkdown()` before `deserializeJson()`. On parse failure, log the raw output and fall back to `local_llm` — never silently drop to IDLE without TTS feedback.
- **Credentials live in NVS, namespace `"jarvis"`, keys ≤15 chars.** Never hardcode WiFi / HA / LLM-backend / Anthropic / TTS / OTA secrets. First-run provisioning is `tools/provision-wifi.py`. See the NVS Schema table in `PLAN.md` for the exact key list; `tts_model` was added in PR #50 and `lemo_host` / `lemo_key` / `lemo_voice` alongside the lemonade TTS provider — all exposed in the captive portal.
- **StackFlow firmware version on the LLM Module drifts.** Currently pinned at `v1.3` (per Phase 1 validation). Re-test after every module FW update — the UART JSON API has shifted across versions.
- **Per-AP WiFi timeout.** Use `WiFiMulti.run(500)` for fast failover; the default is 15–30s. Slot priority is true slot-order (PR #20), not a per-attempt race. After NTP sync, use `configTzTime` so the configured TZ survives (PR-from-`worktree-phase7-validation`).
- **CoreS3 SPI conflict.** SD card and display share SPI on CoreS3. Initialize SD on its own `SPIClass` instance with explicit pin assignment (CS=4, SCK=36, MISO=35, MOSI=37). M5Bus UART to the LLM Module uses Port C: `M5.getPin(port_c_rxd/txd)` — RX=18, TX=17, **not** GPIO16/17.
- **OTA must survive every build.** `ArduinoOTA` is initialized after WiFi in `OtaService` and requires `ota_pass` from NVS. If you remove or forget to call it in a future refactor, the next USB flash silently kills the OTA path — re-flashing requires physical USB. Treat OTA as a load-bearing dependency.

## External endpoints

- **oc-personal runner / personal agent** (LAN-first, Tailscale path also available): `http://192.168.1.178:8080` on the LAN, `https://lobsterboy.tail1c66ec.ts.net` via Tailscale — OpenAI-compatible `POST /v1/chat/completions`, served by `tools/oc-personal-runner/`. `model="oc-personal"` runs a multi-MCP agent loop (12 tools: 6 brain + 6 google) on lobsterboy against the Lemonade `coder` alias; any other model name is proxied to the configured local backend. The device hits the bare LAN port; the Tailscale path is for off-LAN access. **OpenClaw itself was decommissioned 2026-08-30** — the runner is self-contained FastAPI + MCP and never depended on it; the OpenClaw gateway, its user units/timers, and the tailscale-serve `/` route are gone.
- **Local LLM backend**: Lemonade on amd-halo, addressed two different ways. `http://amd-halo:13305` is a Tailscale MagicDNS name, resolvable only from tailnet members — it is the repo default (`config.py`) and works from dev machines. `http://192.168.1.118:13305` is the LAN address, and it is what the **deployed** lobsterboy drop-in actually sets for `OC_BACKEND_URL` (despite a comment in that file saying "amd-halo (Tailscale)") and what the device uses for TTS via NVS `lemo_host` / `kTtsLemonadeHostDefault`. **The CoreS3 cannot resolve the MagicDNS name**, so anything device-side must use the IP. Don't "unify" these onto one name — the hostname doesn't work from the device, and `/healthz` is the authority on what lobsterboy is really using. PR #49 made the proxy provider-agnostic. NVS `oc_host` overrides; `OC_BACKEND_TOKEN` is forwarded as `Bearer` on passthrough (PR #37) — Lemonade requires it.
- **One model serves both paths: `coder`.** Both `OC_AGENT_MODEL` and `OC_PROXY_FORCE_MODEL` point at Lemonade's `coder` alias. The agent loop needs it because its job is routing against a 12-tool schema; the passthrough doesn't tool-call at all (`local_llm` / `claude` are plain chat) but is pinned to the same alias so the stack stays on one resident model. `max_models.llm` is 2, so this leaves a slot free rather than consuming both. Never use `chat`; it does not exist on Lemonade and 404s (fixed 2026-08-30). **Always refer to the alias, never the checkpoint.** `coder` resolves to `Qwen3.8-27B` today, upgraded from Qwen3-Coder-30B with no change on lobsterboy or the device — that re-pointability is the reason aliases are used, and it means the name tells you nothing about which weights answer. `/api/v1/health` on Lemonade is the only authority on the live checkpoint. **Repo default and deployed value can differ:** the live value is whatever `/etc/systemd/system/oc-personal.service.d/override.conf` sets on lobsterboy, and `/healthz` reports it — trust that over any file in this repo.
- **Anthropic (direct)**: `api.anthropic.com` via `AnthropicClient` (PR #19), bearer token in NVS `anth_key` — **severed, not deleted.** `config::kAnthropicDirectEnabled` is `false`, so `IntentRouter` never reaches the client no matter what `anth_key` holds. The client, the NVS key, and the captive-portal field are all intact; flipping the flag restores the path. Nothing in this project calls Claude today: `5116d57` also dropped the Anthropic SDK from the runner. A `claude` intent is now handled exactly like `local_llm` — proxied to Lemonade `coder` — which means it requires LAN or TAILSCALE and **returns the canned "can't reach my brain" line on HOTSPOT_ONLY**, where it used to be the one thing that still worked. That tier has no local backend, so the sever necessarily costs it. Don't read `claude` in a log or an intent table as evidence Claude ran.
- **Home Assistant** (Nabu Casa): `pczxegrio1uswrn1pi0c2cpnfdjomwkx.ui.nabu.casa` — REST API, bearer-token auth from NVS (`ha_token`). `WiFiClientSecure` with `setInsecure()`; cert pinning is a deferred TODO. MQTT broker on HA (`mqtt_host` in NVS); topics `jarvis/state`, `jarvis/command`, `jarvis/speak`.
- **TTS**: `tts_provider` (response) / `tts_proact` (proactive) select per-source, per PR #43. Options are `lemonade`, `openai`, `eleven`, `melotts`.
  - **Lemonade (kokoro)** — `http://<lemo_host>/api/v1/audio/speech`, model `kokoro-v1`, `response_format: "mp3"`. The only **plain-HTTP** provider, so `TtsClient` uses a bare `WiFiClient` for it rather than `WiFiClientSecure`. Credentials are their own NVS keys (`lemo_host` / `lemo_key` / `lemo_voice`) so cloud creds survive alongside. Request `mp3`, never `wav` — the wav variant is 32-bit IEEE float at ~4.8x the size and `AudioPlayer` can't decode it.
  - **Cloud** — OpenAI `/v1/audio/speech` or ElevenLabs `/v1/text-to-speech/<voice_id>`, sharing `tts_api_key` / `tts_voice_id` / `tts_model`.
  - melotts on the LLM Module is the last resort and always speaks, so an error never returns to IDLE silently.
- **STT**: Whisper is served by the same Lemonade instance on amd-halo, but **nothing in this repo calls it.** Speech-to-text is still owned end-to-end by the M5 LLM Module (KWS → ASR over UART); the CoreS3 never touches mic PCM. Routing STT to Whisper is a firmware project, not a provider swap — it needs mic capture, a PSRAM WAV buffer, utterance endpointing, and a multipart upload inside a blocking single-threaded FSM. Deferred to its own spec; see `plans/lemonade-llm-tts.md` §Out of scope. Lemonade having the model available does not shrink that work.
- **Lemonade model loading**: models registered by local path (not pulled from the catalog) cannot be auto-loaded by a chat request — the implicit load resolves the name against the Hugging Face API and 404s, as does `POST /api/v1/pull`. Only an explicit `POST /api/v1/load` works, and it does not survive a `lemond` restart. `oc-personal-runner`'s proxy therefore loads on demand: on a `model_load_error` it POSTs `/api/v1/load` and retries once. Clients that bypass the proxy (Claude Code via CCR) still need a manual load until the registration is repaired on amd-halo.
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

Lobsterboy-side services (`brain-mcp`, `oc-personal-runner`, `google-mcp`, `notifier`) all use the same pattern: `tools/<service>/deploy.sh` + systemd unit templates with `__RUN_USER__` / `__PROJECT_ROOT__` placeholders (PR #32). Deploy from a Tailscale-reachable host; the script handles unit install, venv setup, and `systemctl daemon-reload`. (`morning-brief` uses the same pattern but is currently stopped — see Project state.)

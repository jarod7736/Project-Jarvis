# Project: Jarvis — Voice AI Edge Device

## What This Is

Jarvis is a personal, always-on voice assistant built on embedded hardware. The goal is a device that wakes on a custom wake word, understands natural speech, and can execute actions or answer questions — routing between local and cloud intelligence based on complexity. It is **not** dedicated to any single use case; it's a general-purpose voice-to-information and voice-to-action platform that can be extended over time.

Think: a personal Jarvis that lives on your desk or shelf, works offline for simple tasks, reaches your homelab for complex reasoning, and never fully dies even without internet.

-----

## Hardware

|Component                  |Role                                        |
|---------------------------|--------------------------------------------|
|M5Stack CoreS3 (ESP32-S3)  |Orchestrator, WiFi bridge, display, USB host|
|M5Stack LLM Module (AX630C)|KWS, ASR, TTS — always local via UART/M5Bus |

The LLM Module stacks directly onto the CoreS3 via M5Bus. UART communication at 115200bps 8N1 using JSON packets. Arduino library: `M5Module-LLM`.

The CoreS3 includes a built-in 500mAh LiPo battery and an AXP2101 power management IC. Battery level and charging state are exposed through `M5.Power.getBatteryLevel()` and `M5.Power.isCharging()` in the M5Unified library, which the firmware uses to drive the on-screen power indicator.

-----

## Architecture

```
Mic (LLM Module)
  → KWS — custom wake word, always offline
  → ASR — speech to text, always offline
  → CoreS3 receives transcribed text via UART
  → Intent Router (keyword classifier + Qwen 0.5B as best-effort hint)
      ├── Simple/fast command  → On-device handler (time, date, math, OTA)
      ├── HA action/query      → HA REST API (WiFi)
      ├── Complex reasoning    → OpenClaw / local LLM (LAN/Tailscale)
      ├── Creative / nuanced   → Claude (via OpenClaw, or direct on hotspot)
      ├── Personal query       → 2nd Brain RAG via oc-personal (lobsterboy)
      └── Journal note         → 2nd Brain capture via oc-personal (lobsterboy)
  → Response text → CoreS3
  → TTS — cloud (OpenAI / ElevenLabs) by default, melotts on the LLM Module as fallback
  → CoreS3 display — shows transcript, status, readable data
```

**Key principle:** The LLM Module's Qwen model is a *best-effort hint*, not the primary classifier. The Phase 5 retro showed the on-device Qwen variant (`qwen2.5-0.5b-prefill-20e`) doesn't reliably follow instruction prompts, so a deterministic keyword classifier on the CoreS3 is the primary route, with Qwen consulted as a 4-second optional pre-step. The CoreS3 is a thin orchestrator. Intelligence scales up through the tiers as needed.

-----

## Intelligence Tiers

|Tier|Handler                                |Latency |Use Case                              |
|----|---------------------------------------|--------|--------------------------------------|
|1   |On-device CoreS3 handler               |~instant|Time, date, math, OTA, canned replies |
|2   |Local LLM via OpenClaw / LM Studio     |1–3s LAN|Complex queries, multi-step reasoning |
|3   |Claude via OpenClaw or Anthropic API   |3–6s    |Creative, nuanced, full-intelligence  |
|4   |2nd Brain RAG via `oc-personal`        |3–8s    |Personal knowledge, voice notes       |
|5   |Qwen 0.5B (offline fallback only)      |~instant|No network available                  |

The "OpenClaw" endpoint is now lobsterboy's `oc-personal-runner` (Phase 8) — an OpenAI-compatible front that handles `model="oc-personal"` natively (Claude + brain-mcp agent loop) and proxies every other model name straight through to LM Studio at `192.168.1.108:1234`. CoreS3 sends standard `POST /v1/chat/completions`; the runner picks the path based on the `model` field.

-----

## Backends

- **HA REST API** — light control, lock/unlock, sensor queries, automations
- **OpenClaw / Local LLM** — complex questions, multi-step tasks. Currently `google/gemma-4-e4b` via LM Studio.
- **Claude API** — via OpenClaw as proxy on LAN/Tailscale, or direct to `api.anthropic.com` on phone hotspot
- **2nd Brain (`oc-personal`)** — personal knowledge wiki at `jarod7736/2ndBrain`. Read via `brain_search`, write via `brain_capture`. Runs as an MCP server (`tools/brain-mcp/`) on lobsterboy, fronted by an agent runner (`tools/oc-personal-runner/`) that drives Claude with the MCP tools attached.
- **On-device** — timers, time/date, math, OTA, canned responses

-----

## Connectivity

WiFiMulti with graceful degradation:

```
Home WiFi + LAN           → Full capability, lowest latency
Phone hotspot + Tailscale → Full capability, cellular latency
Phone hotspot only        → Claude API direct (no local LLM, no 2nd Brain)
No network                → Qwen local only (offline tier)
```

`personal_query` and `journal_note` require LAN or Tailscale because the 2nd Brain MCP server lives on lobsterboy. On HOTSPOT_ONLY / OFFLINE the firmware short-circuits to `kErrPersonalOffline` ("I can't reach my notes right now.") rather than burning HTTP timeout budget on a doomed call.

Credentials stored in ESP32 NVS (non-volatile storage), not hardcoded.  
HA long-lived token, OpenClaw URL, and TTS API key all live in NVS.

-----

## Display Usage (CoreS3 2" Screen)

- ASR transcript — show what it heard
- Status indicator — Listening / Thinking / Speaking
- Battery indicator — battery level (percentage or icon) plus a charging-state glyph (e.g. lightning bolt when on USB power, plain icon on battery). Polled from the AXP2101 every few seconds via M5Unified.
- Readable data — temperatures, entity states, timers
- Animation/waveform during TTS playback

-----

## Project Plan

Detailed per-phase design, retros, and validation gates live in [`PLAN.md`](PLAN.md). The summary below tracks high-level state.

### Phase 1 — Hardware Validation ✅

- [x] Receive and unbox CoreS3 + LLM Module
- [x] Stack modules, confirm M5Bus connection
- [x] Flash basic M5Module-LLM Arduino example
- [x] Confirm KWS → ASR → TTS pipeline works end to end (the bundled `VoiceAssistant` example was the wrong abstraction for FW v1.6 — see Phase 1 retro; voice-loop wiring built directly in Phase 2)
- [x] Verify UART JSON communication from CoreS3

### Phase 2 — Wake Word & Speech Pipeline ✅

- [ ] Train custom JARVIS wake word (deferred — see config.h note; reverted to bundled "HELLO" pending KWS-setup debug)
- [x] Flash KWS model to LLM Module (bundled HELLO works; arbitrary all-caps words supported by sherpa-onnx)
- [x] Tune ASR sensitivity for target environment
- [x] Display ASR transcript on CoreS3 screen
- [x] Add status indicators (listening/thinking/speaking states)
- [x] Add battery level and charging-state indicator to the display

### Phase 3 — WiFi & Basic Connectivity ✅

- [x] Implement WiFiMulti with home SSID + phone hotspot fallback (with true slot-order priority)
- [x] Store credentials in NVS (with USB-Serial provisioning fallback + captive-portal config UI)
- [x] Add connectivity status to display
- [x] Tier detection: LAN / TAILSCALE / HOTSPOT_ONLY / OFFLINE

### Phase 4 — HA Integration ✅

- [x] Store HA long-lived token in NVS
- [x] Implement HA REST API client on CoreS3
- [x] Hardcode voice commands → HA actions (lights, locks, etc.)
- [x] Implement HA state queries ("is the garage door open?")
- [x] Test end-to-end: wake word → command → HA action → TTS confirmation (validated, ~3s round-trip)

### Phase 5 — Intent Routing ✅

- [x] Design intent classification prompt for Qwen 0.5B
- [x] Implement routing logic on CoreS3 (parse Qwen response, dispatch to backend)
- [x] Build keyword classifier as primary path (Qwen 0.5B is unreliable as classifier — see Phase 5 retro)
- [x] Add fallback handling for low-confidence classifications

### Phase 6 — OpenClaw / Local LLM Integration ✅

- [x] Implement OpenAI-compatible HTTP client on CoreS3 (`net/LLMClient.cpp`)
- [x] Provider-agnostic local backend — Ollama by default (PR #49), LM Studio still supported via NVS `oc_host`; `LM_STUDIO_TOKEN` forwarded as Bearer on passthrough (PR #37)
- [x] Direct Anthropic API fallback for the `claude` intent on HOTSPOT_ONLY tier
- [ ] Streaming responses (deferred indefinitely — not blocking)

### Phase 7 — Polish & Reliability ✅ (closed for hardware-validation purposes)

See [`plans/phase7-validation.md`](plans/phase7-validation.md) for the gate-by-gate validation log.

- [x] SD card logging (query/response pairs with timestamps)
- [x] Cloud TTS routing — OpenAI / ElevenLabs via `net/TtsClient` + `hal/AudioPlayer`, melotts fallback. Per-source provider routing for proactive pushes (PR #43). Quality/PSRAM fixes in PRs #50/#52–#57.
- [x] MQTT integration alongside HA REST (`net/MqttClient`) — `jarvis/state`, `jarvis/command`, `jarvis/speak` (reverse channel for proactive pushes)
- [x] OTA firmware update support (LAN ArduinoOTA + remote `update_fw` voice intent, `net/OtaService`)
- [x] Hardware watchdog (30s, panic on expire)
- [x] Validation gates 1, 2, 3, 6a, 6d ✅; gates 4, 5, 7 (cloud-TTS happy path, fallback, 24h soak) and rows 6b/c/e/f deferred with rationale in the tracker
- [ ] Enclosure / mounting solution

### Phase 8 — 2nd Brain Integration ✅ (closed for hardware-validation purposes)

See [`plans/phase8-2nd-brain-validation.md`](plans/phase8-2nd-brain-validation.md).

- [x] Firmware: `personal_query`, `journal_note`, calendar/email intents (PRs #25, #36) + `kOcPersonalModel` + `kErrPersonalOffline`
- [x] `tools/brain-mcp/` — Python MCP server, 6-tool catalog (`brain_search`, `brain_capture`, `brain_lint`, `brain_list_projects`, `brain_set_next_action`, `brain_ingest_status`)
- [x] `tools/google-mcp/` — Gmail + Calendar MCP, 6-tool catalog
- [x] `tools/oc-personal-runner/` — FastAPI multi-MCP agent (12 tools total), proxies non-personal models to local backend
- [x] Deployed on lobsterboy; Jarvis NVS `oc_host` repointed at the runner
- [x] Validation gates 1–6 ✅; gate 7 (OFFLINE tier real-device break) and gates 8–10 (ingest / lint / sync-collision) deferred

### Post-Phase 8 — Sprint 1 / Sprint 2 features (active)

- [x] **Notifier service** — 3-tier priority router on lobsterboy:8081 (`tools/notifier/`), high pushes via MQTT `jarvis/speak` + Pushover, medium queues to disk and drains on next IDLE (Sprint 1 #1, #14, PR #41)
- [x] **On-device Settings screen** — brightness / volume / mic-gain sliders (PR #41)
- [x] **Morning brief** — scheduled 08:00 focus brief via `oc-personal` + `brain_list_projects` + `gcal_list_events` (`tools/morning-brief/`, Sprint 2 #2, PR #44)
- [x] Morning brief stale-resurface — biases focus toward projects whose `next_action` hasn't moved in 7+ days (Sprint 2 #10, PR #47)
- [x] Multi-MCP runner (brain + google) (PRs #34, #35) + project-tracking tools (PR #33)
- [x] Calendar / email / project utterances routed to `personal_query` (PR #36)

-----

## Key Constraints & Known Limitations

- **Qwen 0.5B** on the LLM Module is the `qwen2.5-0.5b-prefill-20e` variant — it doesn't reliably follow instruction prompts. Used as a 4-second best-effort hint; the deterministic keyword classifier is the primary route. Phase 5 retro has the gory details.
- **ASR accuracy** degrades in noisy environments. ASR also drops the front of fast speech that follows immediately after the wake word — KWS+ASR start latency, see Phase 2 retro.
- **API latency**: HA round-trip ~3 s; OpenClaw/Claude ~1–6 s; `oc-personal` agent loop up to ~10 s when Claude needs multiple `brain_search` calls (capped at 4 inner turns).
- **LLM Module models** are AXERA-proprietary format — cannot load arbitrary HuggingFace models without conversion.
- **CoreS3 memory** is sufficient for orchestration but not heavy local inference.
- **2nd Brain ingestion** is still LLM-driven and runs from the laptop's `/brain-ingest` Claude Code skill, not from the MCP server. `brain_capture` (write voice notes into `raw/`) works from the device today; agentic processing of those notes happens during a manual ingest pass.

-----

## Reference Links

External:

- [M5Stack LLM Module Docs](https://docs.m5stack.com/en/module/Module-llm)
- [M5Module-LLM Arduino API](https://docs.m5stack.com/en/stackflow/module_llm/arduino_api)
- [LLM Module JSON API](https://docs.m5stack.com/en/stackflow/module_llm/api)
- [CoreS3 Docs](https://docs.m5stack.com/en/core/CoreS3)
- OpenClaw endpoint (LAN): `http://192.168.1.178:8080` — the device path. Tailscale fallback: `https://lobsterboy.tail1c66ec.ts.net`.
- HA Nabu Casa: `pczxegrio1uswrn1pi0c2cpnfdjomwkx.ui.nabu.casa`

In-tree:

- [`PLAN.md`](PLAN.md) — phase-by-phase design, retros, NVS schema, error taxonomy, known pitfalls
- [`CLAUDE.md`](CLAUDE.md) — invariants for AI/agent contributors
- [`plans/phase7-validation.md`](plans/phase7-validation.md) + [`plans/phase8-2nd-brain-validation.md`](plans/phase8-2nd-brain-validation.md) — hardware validation gates + results
- [`tools/brain-mcp/`](tools/brain-mcp/) — 2nd Brain MCP server (6 tools)
- [`tools/google-mcp/`](tools/google-mcp/) — Gmail + Calendar MCP server (6 tools)
- [`tools/oc-personal-runner/`](tools/oc-personal-runner/) — FastAPI multi-MCP agent runner; OpenAI-compat front for `oc-personal`
- [`tools/notifier/`](tools/notifier/) — proactive push service with priority router
- [`tools/morning-brief/`](tools/morning-brief/) — scheduled 08:00 brief
- [`tools/provision-wifi.py`](tools/provision-wifi.py) — first-run NVS provisioning over USB Serial

-----

## Status

**Current state:** Phases 1–8 shipped and "closed for hardware-validation purposes" per the per-phase trackers in `plans/`. Active work is Sprint 1/2 proactive-output features (notifier, morning brief, settings screen) — see commit log for the live frontier.

**Open threads:** Phase 7 deferred gates (24-hour soak, cloud-TTS happy-path + fallback formal validation), Phase 8 deferred gates (OFFLINE-tier guard on real device, `brain_ingest` / `brain_lint` / sync-collision), enclosure. Tracked in agentos.

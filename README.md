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
  → TTS (LLM Module melotts, or cloud TTS in Phase 7) — spoken output
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
- [x] Route complex queries to OpenClaw endpoint (gemma-4-e4b via LM Studio)
- [x] Direct Anthropic API fallback for the `claude` intent on HOTSPOT_ONLY tier
- [ ] Streaming responses (deferred — Phase 7 enhancement)

### Phase 7 — Polish & Reliability (in progress)

- [x] SD card logging (query/response pairs with timestamps)
- [ ] Cloud TTS routing with custom voice (OpenAI / ElevenLabs), melotts fallback for offline tier
- [x] MQTT integration alongside HA REST
- [x] OTA firmware update support (LAN ArduinoOTA + remote `update_fw` voice intent)
- [ ] Graceful degradation testing across all connectivity tiers
- [ ] Enclosure / mounting solution

### Phase 8 — 2nd Brain Integration ✅

- [x] Spec phase 8 in PLAN.md (PR #24)
- [x] Firmware: `personal_query` and `journal_note` intents + `kOcPersonalModel` + `kErrPersonalOffline` (PR #25)
- [x] `tools/brain-mcp/` Python MCP server with `brain_search` / `brain_capture` / `brain_lint` / `brain_ingest_status` (PR #26)
- [x] `tools/oc-personal-runner/` FastAPI service: agent loop with Claude + brain-mcp tools, proxies non-personal models to LM Studio (PR #27)
- [ ] Deploy on lobsterboy and validate end-to-end against the live vault
- [ ] Repoint Jarvis NVS `oc_host` at the runner

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
- OpenClaw endpoint: `https://lobsterboy.tail1c66ec.ts.net` (Phase 8 oc-personal-runner)
- HA Nabu Casa: `pczxegrio1uswrn1pi0c2cpnfdjomwkx.ui.nabu.casa`

In-tree:

- [`PLAN.md`](PLAN.md) — phase-by-phase design, retros, NVS schema, error taxonomy, known pitfalls
- [`CLAUDE.md`](CLAUDE.md) — invariants for AI/agent contributors
- [`docs/reference/`](docs/reference/) — version-pinned local copies of M5Stack / AX630C reference material
- [`tools/brain-mcp/`](tools/brain-mcp/) — Python MCP server exposing the 2nd Brain wiki to OpenClaw
- [`tools/oc-personal-runner/`](tools/oc-personal-runner/) — FastAPI front for the `oc-personal` model alias
- [`tools/provision-wifi.py`](tools/provision-wifi.py) — first-run NVS provisioning over USB Serial

-----

## Status

**Current state:** Phases 1–6 hardware-validated end-to-end. Phase 8 (2nd Brain integration) landed across PRs #24/#25/#26/#27 and is awaiting deploy on lobsterboy. Phase 7 polish (cloud TTS, degradation matrix, enclosure) is the remaining open thread.

**Next action:** Deploy `tools/oc-personal-runner/` on lobsterboy via its `deploy.sh`, repoint Jarvis NVS `oc_host` at the runner, validate `"what do I know about kettlebells"` and `"note that I called the plumber"` end-to-end on hardware.

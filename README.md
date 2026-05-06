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
  → Intent Router (Qwen 0.5B on LLM Module)
      ├── Simple/fast command → Qwen handles locally
      ├── HA action/query    → HA REST API (WiFi)
      ├── Complex reasoning  → OpenClaw / local LLM (WiFi/Tailscale)
      └── Needs full AI      → Claude via OpenClaw
  → Response text → CoreS3
  → TTS (LLM Module) — spoken output, always offline
  → CoreS3 display — shows transcript, status, readable data
```

**Key principle:** The LLM Module's Qwen model is a *router and formatter*, not the primary intelligence. The CoreS3 is a thin orchestrator. Intelligence scales up through the tiers as needed.

-----

## Intelligence Tiers

|Tier|Handler                     |Latency |Use Case                              |
|----|----------------------------|--------|--------------------------------------|
|1   |Qwen 0.5B (local)           |~instant|Simple commands, intent classification|
|2   |Local LLM via OpenClaw      |1–3s LAN|Complex queries, multi-step reasoning |
|3   |Claude via OpenClaw         |3–6s    |Anything needing full intelligence    |
|4   |Qwen only (offline fallback)|~instant|No network available                  |

OpenClaw endpoint: `https://lobsterboy.tail1c66ec.ts.net` (Tailscale Serve)  
OpenClaw exposes an OpenAI-compatible API — CoreS3 sends standard `POST /v1/chat/completions`.

-----

## Backends

- **HA REST API** — light control, lock/unlock, sensor queries, automations
- **OpenClaw / Local LLM** — complex questions, memory, multi-step tasks
- **Claude API** — via OpenClaw as proxy
- **Lightweight HTTP APIs** — weather, time, etc.
- **On-device** — timers, reminders, canned responses

-----

## Connectivity

WiFiMulti with graceful degradation:

```
Home WiFi + LAN          → Full capability, lowest latency
Phone hotspot + Tailscale → Full capability, cellular latency
Phone hotspot only        → Claude API direct, no local LLM
No network               → Qwen local only (Tier 4)
```

Credentials stored in ESP32 NVS (non-volatile storage), not hardcoded.  
HA long-lived token also stored in NVS.

-----

## Display Usage (CoreS3 2" Screen)

- ASR transcript — show what it heard
- Status indicator — Listening / Thinking / Speaking
- Battery indicator — battery level (percentage or icon) plus a charging-state glyph (e.g. lightning bolt when on USB power, plain icon on battery). Polled from the AXP2101 every few seconds via M5Unified.
- Readable data — temperatures, entity states, timers
- Animation/waveform during TTS playback

-----

## Project Plan

### Phase 1 — Hardware Validation

- [ ] Receive and unbox CoreS3 + LLM Module
- [ ] Stack modules, confirm M5Bus connection
- [ ] Flash basic M5Module-LLM Arduino example
- [ ] Confirm KWS → ASR → TTS pipeline works end to end
- [ ] Verify UART JSON communication from CoreS3

### Phase 2 — Wake Word & Speech Pipeline

- [ ] Train custom wake word via M5Stack KWS toolchain
- [ ] Flash custom KWS model to LLM Module
- [ ] Tune ASR sensitivity for target environment
- [ ] Display ASR transcript on CoreS3 screen
- [ ] Add status indicators (listening/thinking/speaking states)
- [x] Add battery level and charging-state indicator to the display (read AXP2101 via `M5.Power`; refresh on a timer; low-battery warning threshold)

### Phase 3 — WiFi & Basic Connectivity

- [ ] Implement WiFiMulti with home SSID + phone hotspot fallback
- [ ] Store credentials in NVS
- [ ] Add connectivity status to display
- [ ] Test Tailscale reachability to OpenClaw from phone hotspot

### Phase 4 — HA Integration

- [ ] Store HA long-lived token in NVS
- [ ] Implement HA REST API client on CoreS3
- [ ] Hardcode 5–10 voice commands → HA actions (lights, locks, etc.)
- [ ] Implement HA state queries ("is the garage door open?")
- [ ] Test end-to-end: wake word → command → HA action → TTS confirmation

### Phase 5 — Intent Routing via Qwen

- [ ] Design intent classification prompt for Qwen 0.5B
- [ ] Implement routing logic on CoreS3 (parse Qwen response, dispatch to backend)
- [ ] Test routing accuracy across command categories
- [ ] Add fallback handling for low-confidence classifications

### Phase 6 — OpenClaw / Local LLM Integration

- [ ] Implement OpenAI-compatible HTTP client on CoreS3
- [ ] Route complex queries to OpenClaw endpoint
- [ ] Handle streaming vs. non-streaming responses
- [ ] Test latency and tune timeout thresholds

### Phase 7 — Polish & Reliability

- [x] SD card logging (query/response pairs with timestamps)
- [ ] Cloud TTS routing with custom voice (OpenAI / ElevenLabs), melotts fallback for offline tier
- [ ] MQTT integration alongside HA REST
- [ ] OTA firmware update support
- [ ] Graceful degradation testing across all connectivity tiers
- [ ] Enclosure / mounting solution

-----

## Key Constraints & Known Limitations

- **Qwen 0.5B** will hallucinate on complex or ambiguous queries — treat it as a router only
- **ASR accuracy** degrades in noisy environments (shop, kitchen background noise)
- **API latency** for OpenClaw/Claude calls will be 1–6s — need audio/visual feedback during wait
- **LLM Module models** are AXERA-proprietary format — cannot load arbitrary HuggingFace models without conversion
- **CoreS3 memory** is sufficient for orchestration but not heavy local inference

-----

## Reference Links

- [M5Stack LLM Module Docs](https://docs.m5stack.com/en/module/Module-llm)
- [M5Module-LLM Arduino API](https://docs.m5stack.com/en/stackflow/module_llm/arduino_api)
- [LLM Module JSON API](https://docs.m5stack.com/en/stackflow/module_llm/api)
- [CoreS3 Docs](https://docs.m5stack.com/en/core/CoreS3)
- OpenClaw endpoint: `https://lobsterboy.tail1c66ec.ts.net`
- HA Nabu Casa: `pczxegrio1uswrn1pi0c2cpnfdjomwkx.ui.nabu.casa`

-----

## Status

**Current state:** Pre-hardware. Planning complete. Hardware en route.  
**Next action:** Phase 1 — hardware validation once CoreS3 + LLM Module arrive.

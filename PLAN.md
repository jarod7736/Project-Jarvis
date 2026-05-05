# Project Jarvis — Implementation Plan

This document goes deeper than the README checklist. For each phase: the goal, key technical decisions, concrete files to write, and validation gates that confirm it's working. Intended audience: solo embedded developer who has the hardware and the README context.

---

## Firmware Folder Layout

```
Jarvis/
├── Jarvis.ino                  # Main sketch: setup(), loop(), state machine tick
├── config.h                    # Compile-time constants, pin defs, timeout thresholds,
│                               #   model names, NVS keys, display region coords
│
├── hal/
│   ├── LLMModule.h/.cpp        # Thin wrapper around M5ModuleLLM: init sequence,
│   │                           #   callback registration for KWS/ASR events,
│   │                           #   llm.inference(), tts.speak(), update()
│   └── Display.h/.cpp          # M5GFX/M5Canvas layer: setStatus(), showTranscript(),
│                               #   showResponse(), animateTTS(), clearRegion()
│
├── net/
│   ├── WiFiManager.h/.cpp      # WiFiMulti setup, isReachable(), getConnectivityTier()
│   │                           #   → enum { LAN, TAILSCALE, HOTSPOT_ONLY, OFFLINE }
│   ├── HAClient.h/.cpp         # HA REST: callService(), getState(), bearer auth
│   └── LLMClient.h/.cpp        # OpenAI-compat POST /v1/chat/completions (non-streaming)
│
├── app/
│   ├── NVSConfig.h/.cpp        # Preferences wrapper: load/save all credentials,
│   │                           #   first-run provisioning via USB Serial JSON
│   ├── IntentRouter.h/.cpp     # Qwen JSON output → Route enum → dispatch to backend
│   ├── CommandHandler.h/.cpp   # Phase 4 hardcoded HA command table; on-device handlers
│   └── Logger.h/.cpp           # SD card append-log: JSONL records per query
│
└── prompts/
    └── intent_prompt.h         # PROGMEM const: system prompt for Qwen intent classifier
```

**Key decisions baked in:**
- No RTOS. `loop()` drives a simple FSM. `M5ModuleLLM::update()` polls UART callbacks.
- HTTP calls are blocking — display must show "Thinking..." before the call, not during.
- PSRAM (8MB) for large JSON buffers. Any buffer over 512 bytes: `ps_malloc()` or `DynamicJsonDocument` (which uses heap extended into PSRAM).
- Non-streaming HTTP responses for simplicity. Streaming is a Phase 7 enhancement.

---

## Library Dependencies

Install via Arduino Library Manager:

| Library | Source |
|---|---|
| `M5CoreS3` | M5Stack (board HAL, power, IMU) |
| `M5Module-LLM` | M5Stack (UART StackFlow wrapper) |
| `M5GFX` | M5Stack (bundled with M5CoreS3) |
| `ArduinoJson` v7.x | Benoit Blanchon |
| `WiFiMulti` | Bundled with esp32 core |
| `Preferences` | Bundled with esp32 core (NVS) |
| `HTTPClient` | Bundled with esp32 core |
| `SD` | Bundled with esp32 core |
| `ArduinoOTA` | Bundled with esp32 core (Phase 7) |

Board: `M5Stack-CoreS3` via M5Stack Arduino board manager URL.  
Pin note: SD card on CoreS3 — CS=4, SCK=36, MISO=35, MOSI=37. M5Bus UART is Serial2 (TX=GPIO16, RX=GPIO17 — confirm against M5Module-LLM source).

---

## State Machine

States live in `state_machine.h`. The FSM is ticked once per `loop()` iteration via `tickStateMachine()`. Callbacks (KWS fired, ASR ready, TTS done) only set flags — they never block or transition state directly.

```
IDLE
  → LISTENING       (KWS wake word detected)

LISTENING
  → TRANSCRIBING    (speech detected by ASR)
  → IDLE            (ASR timeout 10s — no speech)

TRANSCRIBING
  → ROUTING         (ASR complete, transcript ready)

ROUTING
  → FETCHING        (intent requires network call)
  → SPEAKING        (on-device or Qwen-only response ready)
  → ERROR           (intent parse failed after retry)

FETCHING
  → SPEAKING        (response received)
  → ERROR           (timeout 15s or HTTP error)

SPEAKING
  → IDLE            (TTS done callback fires)

ERROR
  → IDLE            (after TTS error message plays)
```

---

## NVS Schema

Namespace: `"jarvis"` (Preferences). All keys ≤15 chars.

| Key | Contents |
|---|---|
| `wifi0_ssid` | Home SSID |
| `wifi0_pass` | Home password |
| `wifi1_ssid` | Phone hotspot SSID |
| `wifi1_pass` | Phone hotspot password |
| `ha_token` | HA long-lived access token |
| `ha_host` | HA host URL (Nabu Casa or local IP) |
| `oc_host` | OpenClaw Tailscale URL |
| `oc_key` | OpenClaw API key |
| `mqtt_host` | MQTT broker IP (Phase 7) |
| `fw_url` | OTA firmware URL (Phase 7) |

First-run: if `wifi0_ssid` is empty, enter provisioning mode — display "Awaiting config via USB", read a JSON blob from `Serial` and write keys to NVS, then reboot.

---

## UART JSON Packet Schema (LLM Module)

Reference: [LLM Module JSON API](https://docs.m5stack.com/en/stackflow/module_llm/api). Keep LLM Module StackFlow firmware version pinned as a comment in `config.h` — API behavior has shifted across versions.

Key actions used:
- `kws.setup` / `kws.inference` — wake word detection
- `asr.setup` / `asr.inference` — speech-to-text
- `llm.setup` / `llm.inference` — Qwen 0.5B (intent classification + offline responses)
- `tts.setup` / `tts.inference` — text-to-speech output

---

## Intent Router Prompt

Stored in `prompts/intent_prompt.h` as `PROGMEM`. Design rules:
1. Hard-constrain the output schema — Qwen 0.5B needs an explicit example, not just "reply in JSON".
2. Provide ≥5 few-shot examples covering boundary cases.
3. Entity extraction is opportunistic — if wrong, `CommandHandler` falls back to keyword matching.

```
You are an intent classifier. Reply ONLY with a JSON object, no explanation.

Schema: {"intent": <string>, "entity": <string|null>, "query": <string|null>}

Intents:
- "ha_command"  — control a home device (lights, locks, covers, switches)
- "ha_query"    — ask the state of a home device
- "local_llm"   — factual or complex question needing reasoning
- "claude"      — creative, nuanced, or multi-step task
- "on_device"   — timer, reminder, math, time/date

Examples:
User: "turn off the bedroom lights"
{"intent":"ha_command","entity":"light.bedroom","query":null}

User: "is the garage door open"
{"intent":"ha_query","entity":"cover.garage_door","query":null}

User: "explain quantum entanglement simply"
{"intent":"local_llm","entity":null,"query":"explain quantum entanglement simply"}

User: "write me a haiku about coffee"
{"intent":"claude","entity":null,"query":"write me a haiku about coffee"}

User: "set a timer for 10 minutes"
{"intent":"on_device","entity":null,"query":"timer 10 minutes"}
```

**Offline prompt** (Tier 4, separate constant): `"You are a helpful assistant. Answer concisely in 1-2 sentences."` — plain text output, piped directly to TTS. No JSON.

---

## Phase-by-Phase Implementation

### Phase 1 — Hardware Validation

**Goal:** Confirm the hardware stack and UART comms work before writing any application code.

**Key decisions:**
- Flash the bundled `VoiceAssistant` example from `M5Module-LLM` — it exercises KWS→ASR→LLM→TTS in ~20 lines.
- Confirm UART pins via library source before assuming GPIO16/17.
- Call `module_llm.sys.ping()` to retrieve and log the StackFlow firmware version — pin it in `config.h`.

**Tasks:**
- Install M5Stack Arduino core + `M5Module-LLM` via Library Manager
- Flash example sketch; open Serial Monitor
- Confirm raw UART JSON packets appear for ASR events
- Confirm TTS plays audio through the module's speaker
- Confirm CoreS3 display initializes (`M5.Display.print` test)

**Validation:**
- Serial shows `{"type":"asr","text":"..."}` on speech
- TTS plays back a synthesized response
- Display shows text without artifacts

**Dependency:** Hardware in hand. Nothing else.

#### Phase 1 retro (2026-05-04)

The bundled `M5ModuleLLM_VoiceAssistant` preset **does not work end-to-end** against the current StackFlow stack on the LLM Module. Tested with Arduino library `M5Module-LLM v1.7.0` (latest as of 2026-05) against base image `M5_LLM_ubuntu22.04_20251121` and StackFlow packages at `llm-llm 1.12, llm-melotts 1.11, llm-vlm 1.11, llm-asr 1.10, llm-kws 1.11, llm-audio 1.9, lib-llm 1.8, llm-sys 1.6`. Two distinct incompatibilities, both reproducible:

1. **`api_melotts.cpp setup()` response timeout is hardcoded to 15000ms.** The current `llm-melotts` (both 1.9 and 1.11) loads a ~465K-entry English lexicon that takes ~16s of CPU time alone (longer wall-clock). `voice_assistant.begin()` returns `-99` (`MODULE_LLM_ERROR_NONE` — "no work_id received in time"), the CoreS3 retries forever. Patching that single constant to `60000` gets past the symptom.

2. **After the timeout patch, `audio.setup` no longer auto-starts the mic capture pipeline.** `voice_assistant.begin()` completes successfully and the display shows "Voice assistant ready," but `llm_audio` and `llm_kws` daemons sit at **0.0% CPU forever** — no PCM ever flows from the mic, KWS never sees audio, never detects the wake word, never triggers ASR. Verified by `/tmp/llm/` socket inventory: ASR/LLM/melotts output sockets are present, KWS socket and `pcm.cap.socket` are missing. The `voice_assistant` preset doesn't send the explicit `audio.cap` (or equivalent capture-start) command that this version of `llm-audio` requires.

What works at the layer below: `module_llm.begin()`, `module_llm.checkConnection()`, `module_llm.sys.version()`, and individual `api_audio` / `api_kws` / `api_asr` / `api_llm` / `api_melotts` `setup()` calls (with timeout patches where needed).

**Tried and didn't help:** rolling the 7 changed StackFlow packages back to the Nov 2025 base-image versions. The preset still fails identically — the audio-capture-trigger gap is in the base image too, not just the May 2026 candidates.

**Phase 1 hardware validation is conceptually satisfied** — board enumerates, USB CDC works on COM9, M5Bus UART JSON traffic flows in both directions, services run, melotts loads its dictionary, llm loads qwen2.5. The bundled sketch is just the wrong abstraction for this device's firmware version.

**Scope folded into Phase 2:** Phase 2 must also build the voice-loop wiring directly from `module_llm.audio.setup`, `module_llm.kws.setup`, `module_llm.asr.setup`, `module_llm.llm.setup`, `module_llm.melotts.setup` plus an explicit `audio.cap` start — replacing what `voice_assistant.begin()` was supposed to do. Custom JARVIS KWS was already Phase 2's main goal; this expands it to "custom KWS + custom voice-loop wiring."

---

### Phase 2 — Wake Word & Speech Pipeline

**Goal:** Custom wake word + stable ASR → transcript on display.

**Key decisions:**

- **Wake word training:** Use M5Stack's browser KWS tool. Record 20–30 samples in the target environment. Target word: "JARVIS" (two syllables, low false-positive risk). Flash the exported `.bin` to the LLM Module via the StackFlow updater (USB-C directly to the LLM Module's port, not CoreS3).

- **ASR callback architecture:** Register with `module_llm.asr.setOnDataCallback()`. Callback sets `g_transcript` and `g_asrReady = true` — never block or call TTS inside a callback.

- **Display regions** (define in `config.h`):
  - `STATUS_BAR_Y=0`, h=30px — state label (LISTENING / THINKING / SPEAKING)
  - `TRANSCRIPT_Y=35`, h=60px — scrolling ASR text
  - `RESPONSE_Y=100`, h=100px — response body
  - `FOOTER_Y=210`, h=30px — WiFi tier + clock

  Use `M5Canvas` (sprite) for the transcript region to avoid display flicker on partial updates.

**Files created:** `hal/LLMModule.h/.cpp`, `hal/Display.h/.cpp`, skeleton `Jarvis.ino`

**Key `LLMModule` API:**
```cpp
void begin(HardwareSerial* serial);
void update();                        // call every loop()
void setOnWake(std::function<void()>);
void setOnTranscript(std::function<void(String)>);
void speak(const String& text);
```

**Validation:**
- Say "JARVIS" → display switches to "LISTENING" within 500ms
- Speak a sentence → transcript appears on display
- 10s silence after wake → returns to IDLE (no hang)
- Call `speak("test")` directly in `setup()` → TTS plays audio

**Known pitfall:** TTS and ASR cannot run simultaneously. Drive `SPEAKING→IDLE` only from the TTS-done callback, not a timer.

**Dependency:** Phase 1 complete.

---

### Phase 3 — WiFi & Basic Connectivity

**Goal:** Reliable multi-network WiFi with connectivity tier awareness; all credentials in NVS.

**Key decisions:**

- **Tier detection** after `WiFiMulti.run()` succeeds:
  1. TCP connect to `ha_host:8123`, 2s timeout → `LAN`
  2. TCP connect to `oc_host:443`, 3s timeout → `TAILSCALE`
  3. Otherwise → `HOTSPOT_ONLY`
  4. `WiFiMulti.run()` fails → `OFFLINE`

  Store in `g_tier` (global enum). Re-check every 30s via `millis()` comparison.

- **WiFiMulti failover speed:** Pass per-AP timeout: `WiFiMulti.run(500)` to fail faster between candidates. Default is very slow.

- **Display footer:** Show tier abbreviation (LAN / TS / HOT / OFF) + `WiFi.RSSI()`.

**Files created:** `app/NVSConfig.h/.cpp`, `net/WiFiManager.h/.cpp`

**Key `WiFiManager` API:**
```cpp
void begin();
ConnectivityTier getConnectivityTier();
bool isReachable(const char* host, uint16_t port, uint32_t timeoutMs);
```

**Validation:**
- No NVS data → display shows "Awaiting config via USB"
- Send provisioning JSON via Serial → connects to home WiFi
- Pull home WiFi → connects to hotspot within 10s
- Kill all networks → tier shows OFFLINE, no crash or hang
- Credentials survive reboot

**Known pitfall:** If `wifi0_ssid` is present but wrong, `WiFiMulti` will still try it on every boot. Add a connection-failure counter in NVS: if >5 consecutive fails for an SSID, skip it and re-prompt provisioning for that slot.

**Dependency:** Phase 2 (display, sketch skeleton).

---

### Phase 4 — Home Assistant Integration

**Goal:** Voice → HA action → TTS confirmation; voice → HA state query → spoken answer.

**Key decisions:**

- **Command table first** (no Qwen routing yet): static array of structs matched via `String::indexOf()`. Intentionally brittle — gets replaced in Phase 5. This validates `HAClient` independently.

  ```cpp
  struct CommandEntry {
    const char* keyword;
    const char* domain;
    const char* service;
    const char* entity_id;
  };
  ```

- **HTTP client:** `WiFiClientSecure` with `client.setInsecure()` for Nabu Casa HTTPS. (Leave a `TODO` in `HAClient.cpp` for cert pinning.) POST body serialized into `char[256]` on the stack.

- **State query parsing:** `StaticJsonDocument<512>` from `http.getStream()`. Return `doc["state"].as<String>()`.

- **Confirmation TTS:** On success: pick from a small canned array ("Done", "Got it", "Okay"). On state query: `"The " + entity + " is " + state`.

**Files created:** `net/HAClient.h/.cpp`, `app/CommandHandler.h/.cpp`

**Hardcode these 10 commands in `CommandHandler`:**
- "turn on [room] lights" → `light/turn_on`
- "turn off [room] lights" → `light/turn_off`
- "lock the door" → `lock/lock`
- "unlock the door" → `lock/unlock`
- "is the garage open" → `GET /api/states/cover.garage_door`
- "close the garage" → `cover/close_cover`
- "temperature" / "how warm" → `GET /api/states/sensor.living_room_temp`
- "run [automation]" → `automation/trigger`
- "turn on fan" → `fan/turn_on`
- "turn off fan" → `fan/turn_off`

**Validation:**
- "turn off the lights" → HA light turns off → TTS confirms
- "is the garage open" → TTS speaks state
- Wrong HA token → TTS: "I couldn't reach home assistant" (no crash)
- HA host unreachable → graceful TTS fallback
- Full pipeline timing: wake word → command → HA action → TTS

**Dependency:** Phase 3 (WiFi + NVS for HA token/host).

---

### Phase 5 — Intent Routing via Qwen

**Goal:** Replace hardcoded dispatch with Qwen classification; add FSM and fallback handling.

**Key decisions:**

- **Single-word classification initially**, then upgraded to JSON schema (see Intent Router Prompt section). Start simple, then add entity extraction once the basic routing is reliable.

- **Parse-fail handling:** If `deserializeJson()` fails or `intent` key is missing, log the raw Qwen output to SD and fall back to `local_llm`. Always give TTS feedback — never silently return to IDLE.

- **Offline path (Tier 4):** Skip routing entirely. Call `llmModule.llm.inference(transcript)` with the offline system prompt. Pipe result directly to TTS.

- **Strip JSON markdown:** Qwen 0.5B often wraps output in backticks. Add `stripJsonMarkdown(String s)` utility in `IntentRouter.cpp` that removes leading ` ```json ` and trailing ` ``` ` before parsing.

**Files created:** `prompts/intent_prompt.h`, `app/IntentRouter.h/.cpp`, `app/state_machine.h/.cpp`  
**Updated:** `Jarvis.ino` — `loop()` now calls `tickStateMachine()` instead of direct dispatch

**Key `IntentRouter` API:**
```cpp
String route(const String& transcript, ConnectivityTier tier);
// Fires appropriate backend, blocks until response ready, returns response string.
// Never throws — returns ERR_* string on failure for caller to speak via TTS.
```

**Validation:**
- 20 test phrases across all 5 intents; log raw Qwen output to Serial; target ≥90% accuracy
- Malformed Qwen response → falls back gracefully, no crash
- HA routing regression: "turn off the lights" still works end-to-end
- `OFFLINE` tier → Qwen responds via TTS, no HTTP calls attempted
- Qwen inference time: measure, should be <2s for short transcripts

**Dependency:** Phase 4 (HA client), Phase 2 (ASR pipeline; Qwen via UART already initialized).

---

### Phase 6 — OpenClaw / Local LLM Integration

**Goal:** Route complex queries to OpenClaw; handle latency gracefully.

**Key decisions:**

- **Non-streaming HTTP:** `http.useHTTP10(true)` disables chunked transfer encoding. The response arrives fully before parsing. Display "Thinking..." animation before the blocking call — the `loop()` stalls during `http.GET()`/`http.POST()`.

- **Request format:**
  ```json
  {
    "model": "local-model-name",
    "messages": [
      {"role": "system", "content": "You are Jarvis, a concise voice assistant. Reply in 1-3 sentences."},
      {"role": "user", "content": "<transcript>"}
    ],
    "max_tokens": 150,
    "stream": false
  }
  ```
  150 max tokens ≈ 30–40 spoken words — right for voice output. Tune via testing.

- **Response parsing:** `DynamicJsonDocument(4096)` allocated in PSRAM. Extract `doc["choices"][0]["message"]["content"]`. Truncate to 500 chars at sentence boundary before TTS.

- **HTTPS:** `WiFiClientSecure` with `setInsecure()` for Tailscale-issued cert on OpenClaw host.

- **Model routing constants** in `config.h`:
  ```cpp
  #define OC_LOCAL_MODEL  "your-local-model-id"
  #define OC_CLAUDE_MODEL "claude-sonnet-4-6"
  ```

- **Timeout:** 10s HTTP timeout. On timeout → `ERR_LLMCLIENT_TIMEOUT` canned TTS response.

**Files created:** `net/LLMClient.h/.cpp`  
**Updated:** `app/IntentRouter.cpp` — calls `LLMClient::query()` for `local_llm` and `claude` intents

**Key `LLMClient` API:**
```cpp
String query(const String& userPrompt, const char* model);
// Returns response content string, or ERR_* on failure.
// Always calls http.end() — no leaks.
```

**Validation:**
1. Standalone test: call `LLMClient::query()` in `setup()`, print to Serial — confirm JSON parses
2. Measure actual round-trip latency for local LLM (LAN) and Claude (Tailscale)
3. Full pipeline: wake → complex question → OpenClaw response → TTS
4. Truncation: send a question expecting a long answer; confirm TTS gets a clean sentence-boundary cut
5. OpenClaw down → TTS: "I can't reach my brain right now" (no crash)

**Known pitfall:** `WiFiClientSecure` + `HTTPClient` has a memory leak if `http.end()` is not reached. Wrap every exit path (early return on error, timeout) to guarantee `http.end()` fires.

**Dependency:** Phase 5 (FSM + routing table), Phase 3 (WiFiClientSecure, connectivity tier).

---

### Phase 7 — Polish & Reliability

**Goal:** Logging, OTA updates, MQTT, watchdog, graceful degradation testing, enclosure.

**SD Card Logging (`Logger`):**
- Append JSONL to `/jarvis_log.jsonl`: `{"ts":<millis>,"transcript":"...","intent":"...","tier":"...","response":"...","latency_ms":...}`
- Rotate at 5MB: rename to `/jarvis_log_old.jsonl` (overwrites previous)
- Initialize SD separately from M5 SPI bus (CS=4). Failures are silent (log to Serial only).

**OTA Firmware Updates:**
- `ArduinoOTA` for LAN push (Arduino IDE / PlatformIO). Start in `setup()` after WiFi connects; call `ArduinoOTA.handle()` each loop.
- Remote OTA via voice: "update firmware" → `HTTPUpdate::update()` pulls `.bin` from `fw_url` in NVS.
- Display shows "OTA: ready" in footer when update service is running.
- **Critical:** include OTA code in every future build or the OTA path is lost on the next flash.

**MQTT:**
- `PubSubClient` library. Broker address in NVS as `mqtt_host`.
- Publish `jarvis/state` on every state transition.
- Subscribe to `jarvis/command` — enables HA automations to push commands to Jarvis.
- Reconnect logic: if `!mqttClient.connected()`, attempt every 30s (millis-based, not blocking).

**Hardware Watchdog:**
- `esp_task_wdt_init(30, true)` — 30s timeout, panic on expire.
- `esp_task_wdt_reset()` at the top of every `loop()` iteration.
- Catches infinite blocking in HTTP calls where the client timeout didn't fire.

**TTS Animation:**
- During `SPEAKING` state: 5-bar waveform via `fillRect()` on a sprite, bar heights varied via `millis()`-derived pseudo-random. Push sprite every 100ms. Animation stops when TTS-done callback fires.

**Graceful Degradation Test Matrix (manual):**

| Scenario | Expected behavior |
|---|---|
| LAN down → hotspot available | Hotspot connect within 15s, Tailscale comes up, Claude reachable |
| All WiFi down | OFFLINE tier, Qwen responds, no crash |
| HA host unreachable | HA commands: TTS apology; non-HA queries still work |
| OpenClaw unreachable | local_llm/claude intents: TTS fallback, no crash |
| SD card absent | Logger fails silently, all else works |
| LLM Module power-cycled | CoreS3 detects via `checkConnection()` timeout, displays "Module offline", retries init every 5s |

**Files created/updated:** `app/Logger.h/.cpp`, update `net/WiFiManager.cpp` (MQTT reconnect, OTA init), update `Jarvis.ino` (watchdog, OTA handle, MQTT loop, animation tick), update `config.h` (watchdog timeout, log rotation size, MQTT topics).

**Validation:**
1. 24-hour soak: 20+ queries, review SD log — all entries present, no gaps
2. OTA: push new firmware via Arduino IDE OTA, device reboots, SD log continues
3. MQTT: HA dashboard shows `jarvis/state` updating; send command from HA
4. Watchdog test: inject `delay(35000)` in dev build → device reboots
5. Full degradation matrix above

**Dependency:** All prior phases.

---

## Cross-Phase Dependency Graph

```
Phase 1 (Hardware) ──┬──► Phase 2 (Speech Pipeline) ──────────────────┐
                     │                                                  │
                     └──► Phase 3 (WiFi/NVS) ──► Phase 4 (HA) ────────► Phase 5 (Routing)
                                                                                 │
                                                                        Phase 6 (OpenClaw)
                                                                                 │
                                                                        Phase 7 (Polish)
```

Phases 2 and 3 can be developed in parallel after Phase 1 (separate subsystems). Phase 4 requires both. Phase 5 requires Phase 4. Phase 6 requires Phase 5. Phase 7 builds on everything.

---

## Memory Budget

| Allocation | Size | Location |
|---|---|---|
| Arduino/M5 framework | ~180KB | SRAM |
| WiFiClientSecure TLS buffers | ~36KB | SRAM |
| Stack | ~16KB | SRAM |
| Small JSON request buffers | `char[256]` on stack | SRAM |
| Large JSON parse (responses) | `DynamicJsonDocument(4096)` | PSRAM (`ps_malloc`) |
| Intent system prompt | ~800 bytes | Flash (PROGMEM) |
| SD log buffer | `char[512]` | SRAM |
| M5GFX display sprites | 320×30×2 bytes each | PSRAM |

Rule: any buffer over 512 bytes goes in PSRAM. Enable PSRAM in board settings; the ESP32-S3 on CoreS3 has 8MB.

---

## Error Response Taxonomy

Define in `config.h` as `const char*` literals:

| Constant | Spoken text |
|---|---|
| `ERR_NO_NETWORK` | "I'm offline. Ask me something simple." |
| `ERR_HA_UNREACHABLE` | "I couldn't reach home assistant." |
| `ERR_LLMCLIENT_TIMEOUT` | "That's taking too long. Try again." |
| `ERR_INTENT_PARSE` | "I wasn't sure what you meant. Could you rephrase?" |
| `ERR_MODULE_OFFLINE` | "My AI module is restarting." |

All error paths: set `g_state = ERROR`, call `display.setStatus(ERROR)`, `llmModule.speak(ERR_*)`, then transition to `IDLE`.

---

## Known Pitfalls & Mitigations

| Pitfall | Mitigation |
|---|---|
| Qwen wraps JSON in markdown backticks | `stripJsonMarkdown()` in `IntentRouter.cpp` |
| TTS and ASR can't run simultaneously | Drive `SPEAKING→IDLE` from TTS-done callback only |
| `WiFiClientSecure` memory leak if `http.end()` skipped | Guarantee `http.end()` on all exit paths in `LLMClient::query()` |
| Chunked HTTP response from OpenAI-compat API | `http.useHTTP10(true)` forces HTTP/1.0, disables chunking |
| CoreS3 SPI conflict between display and SD | Initialize SD on its own `SPIClass` instance with explicit pin assignment |
| StackFlow firmware version drift | Pin version in `config.h` comment; test after every LLM Module FW update |
| `DynamicJsonDocument` heap fragmentation | Allocate via PSRAM; call `doc.clear()` and let it go out of scope promptly |
| WiFiMulti slow failover (15–30s) | `WiFiMulti.run(500)` — per-AP timeout argument |
| KWS false positives | Record samples in real environment; 1s debounce on second KWS trigger |
| Long responses slow TTS | Hard-cap at `max_tokens: 150` in request + sentence-boundary truncation at 500 chars |
| No way to re-provision credentials post-deploy | USB Serial provisioning mode in Phase 3 is the escape hatch |
| Qwen 0.5B hallucination on complex queries | It's a router only — complex queries always escalate to OpenClaw |

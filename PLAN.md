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
│   ├── Memory.h/.cpp           # On-device user memory: prefs, facts, corrections (SD-backed)
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
| `tts_provider` | `"openai"` / `"eleven"` / `"melotts"` (Phase 7) |
| `tts_voice_id` | Provider voice (e.g. `"onyx"` or ElevenLabs UUID) (Phase 7) |
| `tts_api_key` | Bearer token for cloud TTS (Phase 7) |
| `tts_model` | e.g. `"tts-1"` / `"eleven_turbo_v2_5"` (Phase 7) |

First-run: if `wifi0_ssid` is empty, enter provisioning mode — display "Awaiting config via USB", read a JSON blob from `Serial` and write keys to NVS, then reboot.

---

## On-Device Memory & Learning

User-level memory (preferences, facts, corrections) lives on the **CoreS3 SD card**, not NVS. NVS is reserved for credentials and configuration — small, write-rare, wear-limited. SD handles anything that grows or rewrites often. The LLM Module's SD card is owned by StackFlow on the AX630C and reachable only through the UART JSON API, which exposes model/audio operations, not arbitrary file I/O — treat it as model-weight storage only, not application memory.

### SD Layout

Mounted at `/jarvis/` on the CoreS3 SD card. Uses its own `SPIClass` instance (CS=4, SCK=36, MISO=35, MOSI=37) per the SPI-conflict pitfall.

```
/jarvis/
├── prefs.json          # promoted, stable user preferences (small, always loaded)
├── aliases.json        # surface-form rewrites applied before intent routing
├── facts.jsonl         # long-term facts, append-only, compacted offline
├── corrections.jsonl   # raw correction events, append-only
├── episodes.jsonl      # one line per completed interaction (rolling, capped)
└── index/
    └── tags.json       # tag → [(file, offset)] postings list, rebuilt on boot
```

| File | Format | Write cadence | Read cadence | Cap |
|---|---|---|---|---|
| `prefs.json` | single JSON object | rare (promotion or explicit set) | once at boot, kept in PSRAM | 8 KB |
| `aliases.json` | `{ "back room": "office" }` | rare | once at boot, kept in PSRAM | 4 KB |
| `facts.jsonl` | `{ts,key,value,source,tags[]}` | on promotion | on intent dispatch (top-K) | rotate at 256 KB |
| `corrections.jsonl` | `{ts,utterance,wrong,right,tags[]}` | on each correction | on intent dispatch (top-K) | rotate at 128 KB |
| `episodes.jsonl` | `{ts,utterance,intent,route,ok}` | on each turn | rarely (review/distill) | rotate at 1 MB |
| `index/tags.json` | `{tag:[{file,offset}]}` | rebuild on boot, append on write | every dispatch | PSRAM-resident |

JSONL is used for append-heavy files: a single `f.write(line)` per write, no rewrite, no parser needed for the head. Compaction is offline (one-shot from a maintenance intent or OTA hook).

### Lifecycle

```
boot → Memory::begin()
        ├── mount SD on its own SPIClass
        ├── load prefs.json, aliases.json into PSRAM (resident)
        └── stream tails of facts.jsonl + corrections.jsonl to build tags.json index in PSRAM

LISTENING → ASR transcript ready (g_asrReady)
            ├── Memory::applyAliases(transcript)             # surface rewrite
            └── pass into IntentRouter::route(transcript)

ROUTING → IntentRouter assembles prompt with memory blocks (see below), calls Qwen / escalates
          ├── on dispatch success: Memory::recordEpisode(...)
          └── on user repair within N sec of SPEAKING→IDLE: Memory::recordCorrection(...)
```

All SD I/O happens in dispatch handlers, never in callbacks — same single-threaded FSM rule that governs TTS/ASR. Callbacks set flags only.

### IntentRouter Prompt Assembly

`IntentRouter::route(utterance)` builds the system prompt in this order, capped per tier:

```
[BASE_INTENT_PROMPT]              // static, from prompts/intent_prompt.h
[PREFS_BLOCK]                     // selected keys from prefs.json
[FACTS_BLOCK]                     // top-K facts by tag overlap × recency
[CORRECTIONS_BLOCK]               // top-K (wrong → right) examples
[FEW_SHOT_EXAMPLES]               // static + recent successful (utterance, intent) pairs
[USER]: <utterance>
```

Retrieval is keyword/tag based — no on-device embeddings:

1. Tokenize utterance → lowercase word set, drop stopwords.
2. Look each token up in `tags.json` postings → candidate `(file, offset)` list.
3. Score: `tag_overlap × recency_decay` (e.g. `0.5^(age_days / 30)`).
4. Read top-K lines off SD by direct seek (cheap, no full scan).
5. Drop into prompt; truncate the lowest-scored if over budget.

Per-tier budgets — Qwen has tiny context, and the OFFLINE prompt is plain-text (no JSON), so memory injection is deliberately starved there to avoid breaking the prompt format:

| Tier | PREFS | FACTS | CORRECTIONS | FEW_SHOT |
|---|---|---|---|---|
| LAN/TAILSCALE → Claude | full | top-8 | top-4 | full |
| LAN → OpenClaw local | full | top-4 | top-2 | full |
| Local Qwen (online intent) | 4 keys | top-2 | top-1 | static only |
| OFFLINE Qwen (plain-text) | 2 keys | top-1 | 0 | static only |

### Corrections & Promotion

Two correction signals, both detected by `CommandHandler` / `IntentRouter`:

- **Explicit:** repair utterances ("no, I meant the bedroom") detected before the next IDLE. The original utterance + corrected intent are appended to `corrections.jsonl`.
- **Implicit:** an undo intent or re-issued command within N seconds of the prior action.

Promotion is gated to avoid drift:

- A correction is consulted via retrieval immediately, but is **not** trusted as a preference.
- Promotion to `prefs.json` or `aliases.json` requires either an explicit "always do X" utterance, or N consistent corrections (start with N=3) followed by a confirmation TTS ("Should I always send 'back room' to the office?").
- `forget(query)` purges matching entries from `corrections.jsonl` and `facts.jsonl` and rebuilds the relevant index slices.

### Public Surface (`app/Memory.h`)

```cpp
void begin();
void applyAliases(String& utterance);

struct Context {
  String prefs;
  std::vector<String> facts;
  std::vector<String> corrections;
};
Context buildContext(const String& utterance, ConnectivityTier tier);

void recordEpisode(const String& utterance, const String& intent,
                   const String& route, bool ok);
void recordCorrection(const String& utterance, const String& wrongIntent,
                      const String& rightIntent);
void promote(const String& factOrPref);   // gated by N consistent corrections
void forget(const String& query);          // user-triggered purge
```

`Memory` owns the SD `SPIClass`; nothing else touches the card.

### Why not on-device fine-tuning?

Qwen 0.5B on the LLM Module is loaded by StackFlow as a frozen model; the AX630C exposes inference, not training. The ESP32-S3 has neither the compute nor the framework for backprop. "Learning" here is retrieval-augmented adaptation — corrections are stored on SD and replayed into the prompt at decision time. Model-level improvement (distilling corrections into an updated few-shot block, or a Qwen LoRA) is a Phase 8+ topic, would run on OpenClaw, and would ship to the device via OTA.

### Open Questions

1. **Index persistence.** Rebuild `tags.json` every boot vs. persist + append. Default: rebuild-on-boot until boot time becomes painful.
2. **Promotion threshold.** Explicit-only vs. implicit N=3 with confirmation TTS. Default: both, with explicit always winning.
3. **Forget UX.** "Forget that" — last episode, last correction, or list-and-confirm. Default: last episode + offer "forget everything from today" follow-up.

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

#### Phase 2 retro (2026-05-05)

Hardware-validated against StackFlow FW **v1.6** (drifted up from `v1.3` between Phase 1 and Phase 2 — the Phase 1 retro's specific failure modes don't apply to this firmware). Custom voice-loop wiring works; here's what differs from the v1.0.0 spec and from the Phase 1 retro's hypotheses.

**Confirmed working pipeline:**

```
sys.reset → audio.work → kws.setup → asr.setup → melotts.setup
```

**Protocol details that differ from the v1.0.0 spec:**

1. **Stale daemon state across CoreS3 reboots is real.** The AX630C runs Linux; `kws/asr/llm/melotts` daemons persist across CoreS3 power cycles. Without `sys.reset` first, our `kws.setup` returns work_ids like `kws` (no number) — those are the still-running daemons from a prior session, and the leftover Voice Assistant chain auto-responds to wake events while the host CoreS3 sees nothing on UART. With `sys.reset`, the daemons restart and assign numbered work_ids (`kws.1000`, `asr.1001`, `melotts.1002`) per the v1.0.0 spec. **Always start with `sys.reset(true)`.**

2. **`audio.setup` is deprecated in v1.3+** but `audio.work` (without prior setup) is what actually starts the mic capture pipeline on v1.6. Send raw `{"action":"work","work_id":"audio.1000"}` via `module_llm.msg.sendCmd`. The retro's "audio.cap" naming was wrong; the canonical action name is `work`.

3. **ASR `data.delta` is the FULL running transcript hypothesis, not an incremental fragment.** Empirically verified: accumulating `delta` strings produces nonsense like `"what what what what happening what happening..."` because each message contains "what", " what what", " what what happening", etc. — the running ASR best-guess. Replace, don't accumulate. The arduino_api docs example matches this (it just prints `delta` directly, no buffer).

4. **LISTENING window must extend on each ASR fragment, not be fixed at wake time.** A 10s budget set at wake fires the timeout while the user is still mid-sentence (and ASR is still streaming). Reset the deadline on every ASR fragment so 10s means "10s of silence after wake," not "10s of total speech."

5. **`enkws=true` and chained `input` arrays cause the LLM Module's daemons to auto-route internally.** If you set up KWS+ASR+LLM+TTS and chain them via `input: ["asr.xxx", kws_work_id]`, the chain self-fires on wake and the host never sees the events. For Phase 2 (echo only, no LLM), keep ASR `input = ["sys.pcm", kws_work_id]` and don't set up LLM at all — that way ASR's transcript surfaces over UART for the host to act on.

6. **KWS+ASR start latency drops the front of fast speech.** A user saying "HELLO what time is it" in one breath gets transcribed as "is it" — KWS detects the wake word and ASR comes up too late to catch "what time". User-facing fix is a deliberate pause; longer-term we may need to keep ASR running constantly with `enkws=false` and use KWS purely as a gating signal on the host side.

**TTS error -21 was transient.** First two boot attempts after `sys.reset` returned error code `-21` from `melotts.setup` (not in the documented enum, which goes -1 to -17). Third attempt under identical config succeeded — work_id `melotts.1002` came back from the lib's standard 15s wait, no need for the 60s raw-JSON workaround. Likely a daemon-warmup race on cold start; bake in a fallback (display-only echo) and retry tolerance, but don't fork the lib for the 15s ceiling. The 60s workaround is kept in `setupMelottsWithLongTimeout()` as a second-attempt path because it's cheap insurance.

**`sys.lsmode` from raw JSON does not respond within 3s** on this firmware. Diagnostic-only — left in the code with a warning log and skip-on-timeout.

**Validation gates (PLAN.md:374-378):**

| Gate | Status | Notes |
|---|---|---|
| 1. Wake word → LISTENING in <500ms | ✅ | Bundled HELLO KWS asset; JARVIS training deferred to Phase 2.5 |
| 2. Transcript appears on display | ✅ | After fix #3 (replace not accumulate) and fix #4 (deadline-extend) |
| 3. 10s silence → IDLE | ✅ | LISTENING timeout fires correctly when no speech follows wake |
| 4. TTS plays audio | ⚠️ pending user confirmation | melotts.1002 set up successfully; speak() routes through `module_llm.melotts.inference()` |

**Helper script added:** `scripts/build.sh` — Windows pio against WSL-mounted source via SMB hangs at "Processing cores3"; the helper copies the project to a Windows-local temp dir and runs pio there. Supports `run`, `upload`, `monitor`, `clean`.

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

#### Phase 4 retro (2026-05-05)

Hardware-validated end-to-end. Voice → KWS → ASR → CommandHandler keyword match → HAClient bearer-auth POST → Nabu Casa cloud → HTTP 200 → TTS confirmation. Full round-trip latency informally observed at ~3 seconds (HA call dominates; CoreS3 + ASR are sub-second).

**What stayed simple:**

- The `String::indexOf` keyword match with a static table is fine for Phase 4 and is genuinely throwaway as PLAN.md says — Phase 5's IntentRouter replaces it. Don't grow this table beyond what's needed to validate the HAClient round-trip with the user's actual HA install.
- `WiFiClientSecure::setInsecure()` works against the Nabu Casa cert. Cert pinning stays a deferred TODO.

**Notes from the test pass:**

- `http.useHTTP10(true)` and `http.setTimeout(8000)` together are the right combo. Without `useHTTP10`, the OpenAI-compat APIs return chunked encoding; HA's REST handler has historically also chunked larger state responses.
- `http.end()` is called on every exit path in `HAClient::doRequest()` per CLAUDE.md's WiFiClientSecure-leak rule. Verified by leaving the device looping commands for ~10 minutes — no heap fragmentation, no OOM.
- The `applyProvisioningJson()` function takes a bag-of-keys JSON so future phases (MQTT host, OC token, OTA URL) can extend the same Serial provisioning channel by adding a key — no new flow needed.
- Token never re-enters Serial output after provisioning — `[PROV] Saved ha_token (N chars, value not echoed)` is the most we ever say about it.

**Validation gates (PLAN.md:539-543 above):**

| Gate | Status | Notes |
|---|---|---|
| Service call (light/turn_on, light/turn_off) | ✅ | `light.office_1_light` returned HTTP 200, "Done." / "Got it." rotation worked |
| State query (sensor.*, cover.*) | ⏳ deferred | not exercised in the validation pass; HAClient::getState wired and unit-equivalent to callService |
| Wrong token → graceful error TTS | ⏳ deferred | code path returns kErrHaUnreachable, not exercised on hardware |
| Full timing | ✅ | observed ~3s wake-to-confirmation; dominated by HA call (~1.5-2s round trip via Nabu Casa cloud) |

**Out of scope (left for Phase 5):**

- The brittle keyword table. Real intent classification + entity extraction lands with the Qwen IntentRouter.
- Distinguishing "no match" from "HA unreachable" in the user-facing TTS — Phase 5 unifies both behind the IntentRouter's parse-fail handling.

---

### Phase 5 — Intent Routing via Qwen

**Goal:** Replace hardcoded dispatch with Qwen classification; add FSM and fallback handling.

**Key decisions:**

- **Single-word classification initially**, then upgraded to JSON schema (see Intent Router Prompt section). Start simple, then add entity extraction once the basic routing is reliable.

- **Parse-fail handling:** If `deserializeJson()` fails or `intent` key is missing, log the raw Qwen output to SD and fall back to `local_llm`. Always give TTS feedback — never silently return to IDLE.

- **Offline path (Tier 4):** Skip routing entirely. Call `llmModule.llm.inference(transcript)` with the offline system prompt. Pipe result directly to TTS.

- **Strip JSON markdown:** Qwen 0.5B often wraps output in backticks. Add `stripJsonMarkdown(String s)` utility in `IntentRouter.cpp` that removes leading ` ```json ` and trailing ` ``` ` before parsing.

- **Memory injection:** `IntentRouter` calls `Memory::applyAliases()` on the transcript and `Memory::buildContext(transcript, tier)` to assemble the PREFS/FACTS/CORRECTIONS blocks before the static few-shot examples. See "On-Device Memory & Learning" for the prompt order and per-tier budgets. After dispatch, call `Memory::recordEpisode(...)`; on a repair utterance within N seconds of returning to IDLE, call `Memory::recordCorrection(...)`.

**Files created:** `prompts/intent_prompt.h`, `app/IntentRouter.h/.cpp`, `app/state_machine.h/.cpp`, `app/Memory.h/.cpp`  
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

#### Phase 5 retro (2026-05-05)

The on-device Qwen is **`qwen2.5-0.5b-prefill-20e`** — a *prefill-optimized* variant, not the full chat-tuned model. It does not reliably follow instruction prompts. Even with a 1172-char system prompt installed at `llm.setup()` (per the M5 firmware contract — system prompt at setup, user query at inference), Qwen produced unrelated completion text: *"the text of the Act"*, *"iv:1710.00669.pdf"*, *"non-negative real valued functions"* — clearly the model's pretraining-data top-of-mind, not anything it heard from us. Qwen-as-classifier fails on this device.

**Working architecture: keyword classifier as primary, Qwen as best-effort hint.**

`IntentRouter::route()` tries Qwen first (4s budget) and parses the JSON. On any failure (timeout, empty, unparseable, missing intent field), it falls through to a hand-coded keyword classifier covering all five intents (ha_command, ha_query, on_device, local_llm, claude). The keyword classifier handles the realistic phrasings that map cleanly; the final fallback hits `CommandHandler::dispatch` for anything the keyword classifier doesn't catch but the entity table does.

This is more deterministic than expected and probably stays the design even after Phase 6 — Qwen 0.5B isn't a real classifier, and a keyword tree with five intent classes is simple and debuggable. If model-level intent classification matters later (e.g. for entity disambiguation), it'd want a different model on a different node, not the on-device prefill variant.

**Lib forces the model name.** `api_melotts.cpp:31-44` overwrites the caller's `cfg.model` based on `llm_version` (set globally by `checkConnection()`) and `language`. On v1.6+ with `en_US` it forces `"melotts-en-default"`; older firmware used `"melotts_zh-cn"`. Our raw-JSON fallback path tries en-default first, then zh-cn — the package list (`device_stackflow_versions.md`) shows both model packs installed on this device.

**melotts setup race fix.** Error code -21 from `melotts.setup` was hitting roughly 50/50 across boots. Two fixes resolved it:

1. After `sys.reset(true)` returns, the SYS daemon has confirmed reset but the other daemons (audio/kws/asr/llm/melotts) are still loading. 500ms wasn't enough for melotts (~16s lexicon load on first warmup); bumped to 3s. melotts.setup gets called near the end of the boot chain so it has those 3s plus the ~5s of intervening unit setups before it runs.
2. If lib path fails on attempt 1, wait 3s and retry. If still empty, fall through to raw-JSON paths with both model names. Empirically, attempt 2 succeeds on every boot tested after the warmup bump.

**Validation outcomes vs PLAN.md:574 spec:**

| Gate | Status | Notes |
|---|---|---|
| 20 test phrases ≥90% accuracy via Qwen | ❌ | Qwen unusable as classifier; replaced by keyword path |
| Malformed Qwen → graceful fallback | ✅ | Keyword + CommandHandler chain catches every utterance shape tested |
| HA regression (Phase 4 still works) | ✅ | "turn off the office light" → light/turn_off → 200 OK → "Done." |
| OFFLINE tier → Qwen via TTS | ⏳ deferred | not exercised; offline plain-text path is a small follow-up |
| Qwen inference time | ⚠️ | 4s budget hits the deadline frequently; partial output is unparseable JSON anyway |

**Out of scope (left for Phase 6):**
- The local_llm and claude intent branches return a placeholder ("Phase 6 will let me answer that"). OpenClaw client lands those.
- The Memory module (PLAN.md "On-Device Memory & Learning"): not implemented. Phase 5 ships without prefs/facts/corrections injection. The retrieval+prompt-assembly logic depends on SD card support which Phase 7 brings.

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

**Cloud TTS Routing (custom voice):**

Goal: replace the on-device melotts default voice with a configurable cloud voice — e.g. a Walken-style deep gravelly preset — while keeping on-device melotts as the offline fallback.

Architecture:
- New `net/TtsClient` mirrors `net/LLMClient`: HTTPClient + WiFiClientSecure, OpenAI-style POST to `https://api.openai.com/v1/audio/speech` (cheapest path) **or** ElevenLabs `/v1/text-to-speech/<voice_id>`. Both return an audio stream — request `response_format: "mp3"` for OpenAI, `Accept: audio/mpeg` for ElevenLabs. ~24 kbps stereo MP3 = ~3 KB/s; a 10-word reply is ~30 KB.
- New `hal/AudioPlayer` wraps M5Unified's I2S/Speaker_Class. Decodes MP3 on the fly using `ESP8266Audio` (despite the name, runs on ESP32-S3 — bundled with the platform). Push samples into `M5.Speaker` via `playRaw()`. Emits `onPlayDone` callback when the buffer drains, so the FSM can transition `SPEAKING → IDLE` the same way melotts does today.
- `LLMModule::speak()` becomes a router. Default path: cloud TTS → `AudioPlayer` (richer voice). Fallback: tier=OFFLINE, cloud config missing, or `TtsClient` errors → existing melotts UART path. Single call site in `state_machine.cpp` doesn't change.

NVS keys (extend the schema, all ≤15 chars):
- `tts_provider` — `"openai"` | `"eleven"` | `"melotts"` (force-local). Default `"melotts"` so existing devices behave unchanged on update.
- `tts_voice_id` — provider-specific. OpenAI: `"onyx"` / `"echo"`. ElevenLabs: a voice UUID.
- `tts_api_key` — bearer token. Same provisioning path as `oc_key` (Serial-JSON first-run).
- `tts_model` — e.g. `"tts-1"` (OpenAI) / `"eleven_turbo_v2_5"` (ElevenLabs). Cheaper/faster vs higher-quality is a per-deployment call.

Latency:
- OpenAI `tts-1` first-byte ~300–600 ms over LAN; total ~1–2 s for a short sentence. ElevenLabs Turbo similar. Both are streaming so playback can start before the full payload arrives — implement chunked-download → ring-buffer → I2S so the user hears the first syllable while bytes are still in flight. Without streaming we add 1–2 s on top of the LLM latency, which would push total response past the 6 s comfort threshold.
- Hard timeout: 5 s for first byte, 15 s total. On any timeout, fall back to melotts mid-sentence (visible as a brief glitch but better than dead air).

Voice cloning legal note: do NOT clone real public figures (Walken specifically) without consent — it violates right-of-publicity laws in CA/NY and every reputable TTS provider's ToS. Pick a preset voice that *evokes* the target persona (deep gravelly older male) rather than impersonating a real person. Combined with a prompt-side speech-rhythm hint to the upstream LLM (uneven pauses, odd stress patterns), the impression is 80% there without the legal exposure.

**Graceful Degradation Test Matrix (manual):**

| Scenario | Expected behavior |
|---|---|
| LAN down → hotspot available | Hotspot connect within 15s, Tailscale comes up, Claude reachable |
| All WiFi down | OFFLINE tier, Qwen responds, no crash |
| HA host unreachable | HA commands: TTS apology; non-HA queries still work |
| OpenClaw unreachable | local_llm/claude intents: TTS fallback, no crash |
| SD card absent | Logger fails silently, all else works |
| LLM Module power-cycled | CoreS3 detects via `checkConnection()` timeout, displays "Module offline", retries init every 5s |
| TTS provider unreachable / quota exhausted | Falls back to melotts within one timeout window (≤5 s); SPEAKING glitches but completes |
| `tts_provider="melotts"` (force-local) | Cloud TTS bypassed entirely; behaves like pre-Phase 7 |

**Files created/updated:** `app/Logger.h/.cpp`, `net/TtsClient.h/.cpp`, `hal/AudioPlayer.h/.cpp`, update `hal/LLMModule.cpp` (`speak()` becomes a router with melotts fallback), update `app/NVSConfig.cpp` (new tts_* keys), update `net/WiFiManager.cpp` (MQTT reconnect, OTA init), update `Jarvis.ino` (watchdog, OTA handle, MQTT loop, animation tick), update `config.h` (watchdog timeout, log rotation size, MQTT topics, default tts_provider/voice).

**Validation:**
1. 24-hour soak: 20+ queries, review SD log — all entries present, no gaps
2. OTA: push new firmware via Arduino IDE OTA, device reboots, SD log continues
3. MQTT: HA dashboard shows `jarvis/state` updating; send command from HA
4. Watchdog test: inject `delay(35000)` in dev build → device reboots
5. Full degradation matrix above

**Dependency:** All prior phases.

#### Phase 7 retro (2026-05-12)

Code shipped during the Phase 6 → 8 sprint (Logger, OTA, MQTT, watchdog,
cloud-TTS router) had never been actively exercised on hardware. This
retro covers the validation gate, not the implementation.

**Gates run:**

| # | Gate | Result | Notes |
|---|---|---|---|
| 1 | Watchdog reboot | ✅ | Injected `delay(35000)` after `esp_task_wdt_reset()`. Panicked at exactly 30.0 s on `loopTask (CPU 1)`, clean `abort()` → reboot → recovery to IDLE. |
| 2 | OTA round-trip | ✅ | `pio run -t upload --upload-port 192.168.1.104` from Windows. 1.5 MB in ~30 s wire + ~4 s post-reboot. |
| 3 | MQTT pub/sub | ✅ | Subscriber on lobsterboy saw `jarvis/state THINKING → SPEAKING → IDLE`. `mosquitto_pub jarvis/command "what time is it"` dispatched without wake word per `state_machine.cpp:127`. |
| 6a | SD absent | ✅ | Boot without card; `logExchange()` early-exited cleanly on `g_ok=false`; full FSM cycle. |
| 6d | OpenClaw down | ✅ | `sudo systemctl stop oc-personal`; device gracefully spoke "I can't reach my notes right now"; no crash. |

**Deferred** (not blockers for Phase 7 close):
- **Gate 4** (OpenAI cloud TTS happy path) and **Gate 5** (TTS fallback): `tts_provider` stays on melotts; user opted to defer cloud TTS until after Phase 8.
- **Gate 7** (24-h soak): worth running before any user-facing release; not gate-critical.
- **Gate 6 rows b/c/e/f** (HA unreachable, hotspot failover, all-WiFi-down, LLM Module power-cycle): low marginal value vs. disruption to the home network, or covered structurally by row d.

**Drift discovered:**

1. **Tailscale poisoned `192.168.1.0/24` routing on the dev host.** OTA invitations from Windows went into the tailnet (where the CoreS3 isn't reachable) because lobsterboy advertises the subnet via `--advertise-routes`. Fixed with `tailscale set --accept-routes=false` on the dev box. Won't bite again, but worth flagging: any future LAN-direct work from a Windows tailnet member needs this set, or those routes will poach.
2. **`platformio.ini` needed `upload_protocol = espota` + multi-line `upload_flags`.** The env-var path (`$env:PLATFORMIO_UPLOAD_FLAGS=...`) mangles multi-flag input — PIO captures everything after `--auth=` as the auth value. Two flags in the ini are non-negotiable: `--auth=<ota_pass>` and a fixed `--host_port` (for firewall pinning during early debug; can drop once known good).
3. **TZ bug, fixed mid-gate.** `WiFiManager::kickNtpSync()` did `setenv("TZ", ...)` + `tzset()` + `configTime(0, 0, ...)`. On this Arduino-ESP32 (`3.20017.241212`), `configTime()` overwrites the TZ env to UTC internally, so `getLocalTime()` returned UTC despite the setenv. Replaced both with a single `configTzTime(kTimezoneDefault, kNtpServer)` call (`net/WiFiManager.cpp:28`). Discovered when the time intent answered with UTC for Austin Central.
4. **PSRAM "PSRAM ID read error" on every boot is harmless.** Logs `E (284) opi psram: PSRAM ID read error: 0x00000000` then immediately recovers with `psramInit(): PSRAM enabled`. Cosmetic — not worth muting.
5. **Qwen frequently produces unparseable JSON for the time intent.** Falls through to the keyword classifier, which correctly classifies as `on_device`. Working as designed per the Phase 5 retro, but means the time intent's "correctness" actually flows through the deterministic keyword path, not the LLM.

**State as of this retro:**
- Worktree `phase7-validation` (branch `worktree-phase7-validation`) holds: `plans/phase7-validation.md`, `scripts/phase7/mqtt-watch.sh`, `scripts/phase7/mqtt-send.sh`, and the TZ fix in `src/net/WiFiManager.cpp`.
- Windows checkout (`F:\projects\Project-Jarvis`) holds the same TZ fix (applied manually) plus the `platformio.ini` espota config; the watchdog test patch was applied and reverted before commit.
- Phase 7 is closed for the purposes of unblocking Phase 8 follow-up work. Re-run gate 7 (soak) and gates 4/5 (cloud TTS) before any release that's not on-the-bench dev.

---

### Phase 8 — 2nd Brain Integration

**Goal:** Voice queries against the user's personal knowledge wiki, and voice notes that drop into the existing `raw/` ingestion pipeline. Reuses the three Claude Code skills already maintained against `jarod7736/2ndBrain` — `brain-ingest`, `brain-lint`, `brain-query` — by exposing them as MCP tools to OpenClaw.

**Architectural premise:**

The 2nd Brain is a private GitHub repo (`jarod7736/2ndBrain`, branch `main`). The user edits via Obsidian on a Synology SMB share; the Obsidian Git plugin on the laptop is what pushes to GitHub. Today's `brain-query` skill *already* reads from GitHub (via GitHub MCP) — so GitHub is the implicit single source of truth for the queryable wiki, and we lean into that. No SMB mount on the always-on host. No data migration.

```
                ┌──────────────────────────────────────────┐
                │           jarod7736/2ndBrain              │
                │           GitHub  (canonical)             │
                └──────────────────────────────────────────┘
                  ▲              ▲                ▲
                  │ push         │ push           │ pull (cron 5min)
                  │              │                │ push (on capture)
   ┌──────────────┴───┐  ┌───────┴────────┐  ┌────┴───────────────┐
   │ Laptop Obsidian  │  │ Phone Obsidian │  │     Lobsterboy     │
   │ + Git plugin     │  │ via Synology   │  │  /srv/2ndbrain     │
   │ (writes to       │  │ Drive (writes  │  │  (working clone)   │
   │ Synology SMB)    │  │ to share)      │  │                    │
   └──────────────────┘  └────────────────┘  │  ┌──────────────┐  │
                                             │  │ brain-mcp     │  │
                                             │  │ (Python,      │  │
                                             │  │  stdio)       │  │
                                             │  └──────┬───────┘  │
                                             │         │ stdio    │
                                             │  ┌──────▼───────┐  │
                                             │  │ OpenClaw      │  │
                                             │  │ (agent w/     │  │
                                             │  │  brain tools) │  │
                                             │  └──────┬───────┘  │
                                             └─────────┼──────────┘
                                                       │ HTTPS (Tailscale)
                                                       │ /v1/chat/completions
                                                ┌──────▼───────┐
                                                │   Jarvis     │
                                                │   (CoreS3)   │
                                                └──────────────┘
```

**Key decisions:**

- **Lobsterboy is the always-on surface.** Same Tailscale host as OpenClaw — stdio MCP, zero network exposure for the brain server itself. Lobsterboy holds a working clone of `jarod7736/2ndBrain` at `/srv/2ndbrain`. A systemd timer runs `git pull --ff-only` every 5 minutes; every successful brain-capture commits and pushes immediately.

- **Existing Claude Code skills are unchanged.** `brain-ingest`, `brain-lint`, `brain-query` keep working from the user's laptop session against GitHub MCP / SMB. The new MCP server is *additive* — same logic, different transport. Cowork sessions and laptop interactive flows still use the skills; Jarvis goes through the MCP server.

- **Four MCP tools exposed by `brain-mcp`:**

  | Tool | Mirrors skill | Behavior |
  |---|---|---|
  | `brain_search(query: str, k: int = 8)` | `brain-query` | Reads `index.md`, scores wiki/ pages by keyword + path heuristics, returns top-k page contents as raw markdown chunks with paths. **No LLM inside the tool** — the calling agent (OpenClaw) does synthesis. Faster, re-runnable, cheaper. |
  | `brain_lint()` | `brain-lint` | Returns structured signals: `{orphans, broken_links, index_gaps, missing_pages, stale_candidates, missing_xrefs, data_gaps}`. Deterministic checks (1, 2, 3, 5) implemented in Python; interpretive checks (4, 6, 7) return raw co-occurrence/date data; the calling agent prose-summarizes if asked. |
  | `brain_ingest(dry_run: bool = False)` | `brain-ingest` | Long-running. Iterates new `raw/` files not yet in `index.md`, calls Claude (via OpenClaw) with the existing ingest prompt for each, writes wiki/ outputs, commits + pushes. Triggered by cron or manually — never on Jarvis's hot path. |
  | `brain_capture(content: str, source: str = "jarvis")` | NEW | Writes `raw/notes/<ISO8601>-<source>.md` with minimal frontmatter (`{source, captured_at, transcript}`). Commits + pushes. No LLM. Sub-second. |

- **Filename for captured notes:** `raw/notes/YYYY-MM-DDTHH-MM-SS-jarvis.md` (colons replaced with dashes for cross-platform safety; ISO-ish but filesystem-safe). Per-second granularity is fine — duplicate seconds get a `-2`, `-3` suffix. Frontmatter conforms to the existing skill convention so brain-ingest needs no awareness this came from Jarvis:
  ```
  ---
  source: jarvis
  captured_at: 2026-05-09T14:32:07Z
  type: note
  ---
  <transcript>
  ```

- **`brain_search` is not a vector search.** Matches the existing `brain-query` skill: read `index.md`, identify candidate pages via filename/title keyword overlap and front-matter tag matches, return raw page contents. The calling LLM does the reasoning. If a vector index is added later (sqlite-vec or chroma reading from `wiki/`), it slots in behind the same tool signature.

- **Jarvis-side: one new model name, two new intents.** Jarvis never talks to the brain MCP directly. Instead, `config.h` gains:
  ```cpp
  #define OC_PERSONAL_MODEL "oc-personal"   // OpenClaw agent loop with brain tools
  ```
  OpenClaw recognizes this model name and runs an agent loop with the brain MCP tools attached, returning the final assistant message via the same OpenAI-compat `/v1/chat/completions` shape `LLMClient::query()` already speaks. No firmware changes to `LLMClient`.

  `IntentRouter` adds two intents:

  - `personal_query` — read-side. Routes to `OC_PERSONAL_MODEL` with the user's transcript as the user message. Examples: "what do I know about kettlebells", "have I read anything on stoicism", "what's in my wiki about the boat project".
  - `journal_note` — write-side. Routes to `OC_PERSONAL_MODEL` with a system prompt biased toward "use brain_capture, then confirm tersely." Examples: "note that I called the plumber", "remember that the boat needs a new bilge pump", "save this to my brain — ...".

- **Tier handling.** Both intents require `LAN` or `TAILSCALE`. On `HOTSPOT_ONLY` or `OFFLINE`, `IntentRouter` short-circuits to `ERR_PERSONAL_OFFLINE` without calling OpenClaw. The on-device Qwen 0.5B has no useful access to the wiki.

- **No new NVS keys.** Brain access is gated by the existing `oc_host` / `oc_key`. Auth between OpenClaw and the brain MCP is stdio (same process tree on lobsterboy) — no token. GitHub auth on lobsterboy uses a deploy key on disk, not anything the firmware sees.

**Files created — `lobsterboy-services/brain-mcp/`** (new repo or directory on lobsterboy, not in Project-Jarvis):

```
brain-mcp/
├── pyproject.toml          # mcp[cli], gitpython, python-frontmatter
├── server.py               # MCP server: tool registrations, stdio transport
├── tools/
│   ├── search.py           # brain_search implementation
│   ├── lint.py             # brain_lint structural checks
│   ├── ingest.py           # brain_ingest — calls OpenClaw for each new file
│   └── capture.py          # brain_capture — write raw/notes/, commit, push
├── vault.py                # path resolution, git pull/commit/push helpers
├── config.py               # VAULT_PATH=/srv/2ndbrain, GITHUB_REMOTE, etc.
└── systemd/
    ├── brain-mcp.service   # Invoked by OpenClaw on demand (stdio); not a long-running daemon
    ├── brain-sync.service  # One-shot: cd /srv/2ndbrain && git pull --ff-only
    └── brain-sync.timer    # Every 5 min
```

The MCP server is a stdio child of OpenClaw, not a network daemon. Spawn-on-demand is fine — MCP tools run inside the OpenClaw agent loop, not a long-lived process.

**Files updated — Project-Jarvis (firmware):**

- `config.h`
  - Add `#define OC_PERSONAL_MODEL "oc-personal"`
  - Add `#define ERR_PERSONAL_OFFLINE "I can't reach my notes right now."`
- `prompts/intent_prompt.h` — extend few-shot examples (3 new):
  ```
  User: "what do I know about kettlebells"
  {"intent":"personal_query","entity":null,"query":"what do I know about kettlebells"}

  User: "note that I called the plumber"
  {"intent":"journal_note","entity":null,"query":"called the plumber"}

  User: "save this to my brain — the boat needs a new bilge pump"
  {"intent":"journal_note","entity":null,"query":"the boat needs a new bilge pump"}
  ```
  Update the intent enum list in the system prompt to include `personal_query` and `journal_note`.
- `app/IntentRouter.cpp`
  - Add dispatch branches for both intents.
  - Tier guard: if `tier != LAN && tier != TAILSCALE`, return `ERR_PERSONAL_OFFLINE` without calling `LLMClient`.
  - Both branches call `LLMClient::query(transcript, OC_PERSONAL_MODEL)`.
  - Per CLAUDE.md, the keyword-classifier fallback added in the Phase 5 retro must also learn these intents — add keyword paths for "what do I know", "have I read", "what's in my wiki", "note that", "remember that", "save this".
- (No changes to `net/LLMClient.cpp`, `app/CommandHandler.cpp`, or NVS schema.)

**OpenClaw-side work** (out of this repo, listed for completeness):

- Register `oc-personal` as a model alias that routes through an agent loop. The loop's tool list = brain MCP server. Underlying LLM = whichever Claude SKU is current (matches `OC_CLAUDE_MODEL` cost/quality). System prompt biases toward terse voice-friendly replies and prefers `brain_capture` for any utterance phrased as a note/save/remember.
- Cap turn count at 4 inner agent steps (search → maybe-search-again → answer; or capture → confirm). Beyond that, OpenClaw returns whatever it has — keeps Jarvis's 10s `LLMClient` timeout meaningful.

**Validation gates:**

| # | Gate | How to verify |
|---|---|---|
| 1 | `brain-mcp` server starts and lists 4 tools | `mcp dev brain-mcp/server.py` shows `brain_search/brain_lint/brain_ingest/brain_capture` |
| 2 | `brain_search("coffee")` returns wiki/ pages | Manual call against a vault that has coffee-tagged sources; chunks contain expected page paths |
| 3 | `brain_capture("test note")` writes a file and pushes | New file in `raw/notes/` on disk; `git log origin/main` on the laptop shows the commit after a pull |
| 4 | OpenClaw end-to-end: POST `/v1/chat/completions` with `model=oc-personal` and a personal question returns a wiki-grounded answer | Use `curl` from lobsterboy first, then from a laptop on Tailscale |
| 5 | Jarvis: "Hey Jarvis, what do I know about kettlebells" → TTS answer derived from wiki | Hardware voice loop; check OpenClaw logs show `brain_search` invoked |
| 6 | Jarvis: "Hey Jarvis, note that I called the plumber" → terse confirm + file lands in `raw/notes/` | Hardware; verify file appears within ~5s of TTS confirm; verify next laptop Obsidian Git pull picks it up |
| 7 | Tier=OFFLINE while saying a personal query → `ERR_PERSONAL_OFFLINE` TTS, no HTTP attempted | Disconnect WiFi, repeat gates 5/6 |
| 8 | `brain_ingest()` processes a new `raw/notes/` file end-to-end | Trigger via `mcp` CLI or scheduled job; verify wiki/sources page created, index.md and log.md updated, commit pushed |
| 9 | `brain_lint()` returns structured signals on a known-bad vault state | Seed an orphan + a broken `[[link]]`; both appear in the response |
| 10 | Sync direction collisions don't corrupt the vault | While brain-capture is writing on lobsterboy, edit any wiki/ file in Obsidian on the laptop. Both push successfully (different files); next pull on either side merges cleanly |

**Known pitfalls:**

- **`brain_ingest` is LLM-driven and slow.** A single ingest call may take 30–120s per source file (Claude reasoning over content). It must never be on Jarvis's hot path. `journal_note` writes via `brain_capture` only; ingestion runs on a separate cron or manual trigger.
- **GitHub push race.** Lobsterboy and the laptop's Obsidian Git plugin can both push to `main`. `brain_capture` only adds new files (timestamped, never collide). Skill-driven edits (ingest) only touch `wiki/`. Manual Obsidian edits touch any file. Mitigation: brain-mcp pulls before every push; on push reject (non-fast-forward), pull-rebase-retry once. Conflicts on the same file are rare but should error loudly rather than auto-resolve — surface to `brain_lint` rather than silently merge.
- **`raw/notes/` filename collisions inside the same second.** Two voice notes captured within 1s get `-2`, `-3` suffixes. Implement before shipping; a single soak test should be enough to flush this out.
- **Cowork sessions.** The laptop's `brain-query` skill explicitly notes Cowork is read-only against the repo. Don't try to "improve" the skill to write — Cowork can't, by design. The MCP server is the write path.
- **Vault path drift.** The skills hardcode `\\synology.holdfast.lan\homes\jarod7736\2nd Brain`. The MCP server uses `/srv/2ndbrain` (the GitHub clone). Same logical content, different paths. Don't share path constants across the two surfaces; let each own its own `config`.
- **Synology is now optional, not canonical.** If the user later turns the NAS off, brain still works — lobsterboy holds a complete clone. Documented as a feature, but flag it: a NAS-down scenario means the laptop loses live multi-device editing, not that Jarvis breaks.
- **brain-mcp deploy key scope.** The lobsterboy deploy key needs *push* access to `jarod7736/2ndBrain` (capture commits push). Read-only deploy keys won't work. Use a dedicated key (not the user's personal SSH key) so it can be rotated independently.

**Dependencies:**

- **Required:** Phase 6 (OpenClaw integration — `LLMClient`, the chat-completions plumbing, `OC_LOCAL_MODEL`/`OC_CLAUDE_MODEL` pattern). Phase 5 (IntentRouter + keyword fallback).
- **Independent of:** Phase 7 (logging, OTA, MQTT, watchdog, custom TTS — none of these gate Phase 8).
- **External (lobsterboy):** existing OpenClaw deployment with ability to register new model aliases that wire to an agent loop with MCP tools attached.
- **External (laptop):** existing Obsidian + Git plugin workflow continues unchanged.

**Open questions to resolve during implementation:**

1. Does OpenClaw already support agent loops with MCP tool attachments, or is that a new build out? If new, that's the long pole — brain-mcp itself is a few hundred lines.
2. Persona/context injection (constant "about-me" snippet prepended to every personal query) — desirable but not in this phase. If wanted, add a `brain_persona()` MCP tool that returns a curated profile note's contents, and have OpenClaw's `oc-personal` system prompt prepend it. Defer until baseline RAG works.
3. Should `brain_ingest` run on a cron on lobsterboy, or stay manual (laptop-driven by the user opening a Claude Code session)? Cron means new voice notes get cleaned up overnight without intervention; manual means the user reviews each ingest pass. Default: manual until trust is established, then cron.

---

## Cross-Phase Dependency Graph

```
Phase 1 (Hardware) ──┬──► Phase 2 (Speech Pipeline) ──────────────────┐
                     │                                                  │
                     └──► Phase 3 (WiFi/NVS) ──► Phase 4 (HA) ────────► Phase 5 (Routing)
                                                                                 │
                                                                        Phase 6 (OpenClaw)
                                                                                 ├──► Phase 7 (Polish)
                                                                                 └──► Phase 8 (2nd Brain)
```

Phases 2 and 3 can be developed in parallel after Phase 1 (separate subsystems). Phase 4 requires both. Phase 5 requires Phase 4. Phase 6 requires Phase 5. Phases 7 and 8 both depend on Phase 6 and are independent of each other — they can ship in either order.

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
| `ERR_PERSONAL_OFFLINE` | "I can't reach my notes right now." |

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
| Correction loops poison memory (ASR misheard, user "corrects" → wrong pair stored) | Distinguish ASR-confidence from intent error; require N=3 consistent corrections before promoting to `prefs.json`; cap `corrections.jsonl` by recency; expose `forget(query)` |
| `brain_ingest` is LLM-driven and slow (30–120s/file) | Never on Jarvis's hot path; `journal_note` writes via `brain_capture` only; ingestion runs on cron or manual trigger |
| GitHub push race between lobsterboy and laptop Obsidian Git | brain-mcp pulls before push; on non-fast-forward, pull-rebase-retry once; `brain_capture` only adds new files (never edits), so collisions are near-zero |
| `raw/notes/` filename collision within same second | Append `-2`, `-3` suffix on stat-exists check before write |
| Brain MCP server vault path drifts from skill paths | Skills use Synology SMB path; MCP server uses `/srv/2ndbrain` (GitHub clone). Don't share path constants — let each surface own its `config` |

---

## Future Work — UI

The current display is functional but minimal: status bar (color-coded by FSM state), transcript region, response region, footer (tier / RSSI / clock / OTA badge), oscilloscope waveform during SPEAKING. Two known gaps to address after Phase 7 closes:

- **Idle screen.** IDLE currently just shows the word "IDLE". Needs a daily-use surface — clock, calendar/weather widget, wake-word hint, optional next-alarm or HA-quick-state strip.
- **Touch input.** CoreS3's touchscreen is wired up but unread. Unlocks: settings menu (re-provisioning without serial / NVS wipe), manual wake fallback when KWS misses, conversation-history scrollback, volume control.

Neither is load-bearing for voice loop correctness — they're UX polish. Defer until validation work (soak, degradation matrix) is closed and the firmware is otherwise shippable.

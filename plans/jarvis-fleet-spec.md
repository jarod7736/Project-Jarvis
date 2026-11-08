# Spec — Jarvis Fleet: thin voice endpoints on a central brain

Date: 2026-11-07
Status: draft. **Supersedes `plans/hermes-mcp-spec.md` / `hermes-mcp-plan.md` as the
primary direction** — those assumed Jarvis *drives* Hermes as a remote trigger;
this makes Jarvis a *transport* into Hermes. Keep them as an appendix for that
option.
Related: `plans/hermes-control.md`, `plans/lemonade-llm-tts.md`.

---

## 1. Goal

A fleet of identical, cheap voice endpoints (M5Stack CoreS3, **no** M5
Module-LLM) placed around the house. Each endpoint is only wake word +
microphone + speaker + network. All reasoning, memory, integrations, STT and
TTS live on `amd-halo` / Hermes.

## 2. Decisions this rests on

1. `amd-halo` is always up — no offline capability is required (confirmed by the
   user).
2. The device should not keep its own brain: no on-device intent, no local HA,
   no local LLM. That work moves to Hermes.
3. The **M5 Module-LLM (AX630C) is retired.** Its four jobs (wake word, ASR,
   Qwen intent, melotts) are each displaced: wake → on-S3 engine, ASR → Whisper,
   intent → Hermes, TTS → kokoro. A second CoreS3 without the module is the
   reference device.
4. **Lemonade is the voice service**: Whisper (STT) and kokoro (TTS), both on
   `amd-halo`. The firmware's kokoro TTS path already exists
   (`net/TtsClient::synthLemonade`, provider `"Lemonade (kokoro, LAN)"`).
5. **Hermes is the brain**: intent, Home Assistant, Google, the 2nd-brain wiki,
   skills, memory, agentic work.

## 3. Architecture

```
┌──── CoreS3 endpoint (×N, no LLM module) ────┐
│  wake word (on-S3)                          │
│  mic capture → endpointing → utterance buf  │
│  device id + room                           │
│  speaker playback (AudioPlayer / M5.Speaker)│
└───────────────────│─────────────────────────┘
                    │ LAN / tailnet, per-device token
                    ▼
┌─────────────── amd-halo ────────────────────┐
│  voice gateway (new, small)                  │
│    STT: Lemonade Whisper                     │
│    TTS: Lemonade kokoro                      │
│    route → Hermes                            │
│    device registry, concurrency              │
└───────────────────│─────────────────────────┘
                    ▼
┌─────────────── Hermes (brain) ───────────────┐
│  HA (hermes-homeassistant), Google, wiki,    │
│  skills, memory, delegation                  │
└──────────────────────────────────────────────┘
```

The gateway may collapse into a Hermes platform/plugin rather than a bespoke
service — open (see §10).

## 4. Device contract

- **Wake word:** on-S3 engine ("Jarvis"); exact engine pending the wake-word
  research lane. Must run always-on with WiFi + display live.
- **Capture:** CoreS3 mic → VAD/endpointing → single-utterance buffer in PSRAM
  (8 MB available). No streaming of idle audio.
- **Transport:** upload one utterance; receive reply text (and/or audio).
- **Playback:** `AudioPlayer` → `M5.Speaker` (existing path).
- **Identity:** `device_id` + `room`, from NVS, sent on every request.
- **Provisioning:** captive portal (existing), trimmed to WiFi + gateway URL +
  device id + room + wake word.
- **Keep:** OTA, status display, mute/hardware button, barge-in/interrupt.

## 5. Gateway contract (amd-halo)

| Method | Path | Purpose |
|---|---|---|
| POST | `/v1/utterance` | `{device_id, room, audio}` → STT → brain → `{text, audio?}` |
| POST | `/v1/speak` | `{device_id \| room \| "all", text}` — proactive push |
| GET | `/v1/health` | gateway + backends reachability |
| GET | `/v1/devices` | registered endpoints and states |

- **Auth:** per-device token.
- **Concurrency:** N devices; queue per device, do **not** serialize globally
  (`oc-personal-runner` serializes with one lock today — that breaks with
  several endpoints).
- **STT/TTS:** Lemonade (Whisper / kokoro); the reply sanitizer (strip markdown)
  and a word/length clamp stay, so TTS never reads `**Paris**`.

## 6. Brain (Hermes)

- Intent, HA, Google, wiki, skills, memory all live in Hermes.
- Wire the existing `brain-mcp` / `google-mcp` as Hermes MCP servers
  (`hermes mcp add`); HA is already present (`hermes-homeassistant`, enabled).
- **Voice-shaped replies:** Hermes must return short, spoken text. If its native
  reply shape is agent-verbose, the gateway clamps/sanitizes (a voice-tuned
  layer), or Hermes gets a device-oriented prompt.
- Session model (one per device/room vs shared) is open — see §10.

## 7. Voice service

- **STT:** Lemonade `Whisper-Large-v3-Turbo` (installed, idle today).
- **TTS:** Lemonade `kokoro-v1` (LAN, MP3, verified); cloud TTS retained as an
  off-LAN fallback via the existing `tts_*` keys.
- Falls back so a dead backend never leaves the device silent.

## 8. Multi-device specifics

- Registry of `device_id` → `room`.
- Notifications: broadcast (all) or targeted (one room), replacing the
  single-device `notifier` assumption.
- Per-device wake-word option (a device could listen for one of several names).

## 9. Reuse map (current repo)

| Keep | Delete | Rewrite |
|---|---|---|
| `net/WiFiManager`, `net/CaptivePortal`, `net/OtaService`, `hal/Display`, `hal/AudioPlayer`, `net/TtsClient`, `hal/SdLogger`, `data/web/*`, PSRAM/partition config | `app/IntentRouter`, `app/CommandHandler`, `prompts/*`, `hal/LLMModule`, `net/AnthropicClient`, `net/HAClient`, `app/MathParser`, most of `app/ConfigSchema` | `app/state_machine` → a simpler audio FSM; `net/LLMClient` → a gateway client |

The module/USB-flash and StackFlow lore in `PLAN.md`/`CLAUDE.md` becomes
historical once the module is retired.

## 10. Open questions

1. **Wake-word engine** — Porcupine vs ESP-SR on the S3 (research lane running).
2. **Audio transport** — upload a buffered WAV per utterance, vs a streaming
   path. Buffered is simpler; streaming is lower-latency.
3. **Where the gateway lives** — a new small service (like `cardputer-bridge`)
   vs a Hermes platform/plugin. Note the existing `cardputer-bridge` is the same
   shape (device → bridge on amd-halo → agents) and probably wants to be one
   service.
4. **Does Hermes accept audio input / emit audio**, or does the gateway own
   STT/TTS and hand Hermes text only?
5. **Session model** — per-device conversation vs one shared thread.
6. **Module disposition** — keep the one module device as a legacy experiment?

## 11. Non-goals

- No offline capability (amd-halo is assumed up).
- No per-device brain.
- No HA-on-device (HA moves to Hermes).
- Not the remote-trigger design (that is `hermes-mcp-spec.md`, now an appendix).

## 12. Risks

- **Multi-device concurrency** on a single box — needs solving in the gateway.
- **Wake-word quality on the S3** — the whole design hinges on it; unproven.
- **Latency** — wake → capture → upload → Whisper → Hermes → kokoro → playback
  is a longer chain than the module's local loop; must stay inside a few seconds.
- **Reply shape** — Hermes is an agent, not a voice device; needs shaping.
- **Revalidation cost** — retiring the app layer discards working behaviour
  (HA control, on-device timers) that must be re-established in Hermes.

## 13. Verification

1. One CoreS3 (no module) wakes on "Jarvis", captures, and gets a spoken reply
   end to end.
2. Round-trip latency measured and acceptable (< ~4 s warm).
3. Two devices addressed independently; per-room targeting works; no global
   serialization stall.
4. HA command via voice reaches `hermes-homeassistant` and acts.
5. Proactive push to one device and to all.
6. Reboot/provision a fresh device from the captive portal alone.

## 14. Project structure (decided 2026-11-07)

**Same repository, not a separate project.** The reusable half is the device
plumbing (`net/WiFiManager`, `net/CaptivePortal`, `net/OtaService`, `hal/Display`,
`hal/AudioPlayer`, `net/TtsClient`, `hal/SdLogger`, `data/web/*`, the PSRAM /
partition config) — hard-won, and a new repo would only copy it, with no way to
merge fixes back. The app layer is deleted either way, and the brain moves into
Hermes, so a fork buys nothing here.

- Build the thin endpoint as a **second PlatformIO env** (`cores3-thin`) in this
  tree.
- Prototype in a **git worktree / branch** so the current device is untouched.
- Split into its own repo only if the fleet becomes a separate product line, or
  the current device must keep evolving untouched.

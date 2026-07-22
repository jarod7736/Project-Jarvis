# Lemonade on amd-halo as Jarvis's primary LLM + TTS

**Date:** 2026-07-22
**Status:** approved, pending implementation
**Scope:** LLM routing + TTS. STT is explicitly deferred to its own spec.

## Problem

Project-Jarvis should use Lemonade Server on `amd-halo` as its primary LLM, and
use it for TTS and STT as well.

Investigation changed the shape of the task:

1. **The LLM half is already deployed but broken.** lobsterboy's `oc-personal`
   systemd override already sets `OC_BACKEND_URL=http://amd-halo:13305` and
   `OC_PROXY_FORCE_MODEL=gpt-oss-120b-Q4_K_M`. But that model fails to load:

   ```
   model_load_error: Failed to fetch model info from Hugging Face API (status: 404)
   ```

   End-to-end, `oc-personal` returns `(upstream: backend HTTP 500)`. Jarvis's
   `local_llm` intent is dead in production today.

2. **The repo is behind the deployment.** `src/config.h`,
   `tools/oc-personal-runner/.../config.py`, and `CLAUDE.md` all still describe
   Ollama at `192.168.1.108:11434` with `gemma3n:e4b`.

3. **STT is not symmetric with TTS.** The CoreS3 never touches mic PCM — the M5
   LLM Module owns the whole audio→KWS→ASR chain and the host polls text over
   UART. Routing STT to Whisper means mic capture, a PSRAM WAV buffer, utterance
   endpointing, and a multipart upload inside a single-threaded FSM where
   `http.POST()` stalls `loop()`. That is a firmware project, not a provider
   swap, so it is deferred.

## Verified facts

Measured against the live server during design, not assumed:

| Fact | Value |
|---|---|
| amd-halo LAN address | `192.168.1.118:13305` (the vault runbook's `.61` is stale and does not answer) |
| API base | `/api/v1`, `Authorization: Bearer <key>` |
| Lemonade version | `10.5.1` |
| `max_models` | `llm: 2`, `tts: 2`, `transcription: 2` |
| Resident at design time | `Qwen3-Coder-30B`, 2 embedding models, `Z-Image-Turbo`, **`kokoro-v1`** |
| kokoro MP3 output | `200`, 97 KB, `audio/mpeg`, 24 kHz mono 160 kbps, ~1.9 s for a full sentence |
| kokoro WAV output | 465 KB, IEEE float — rejected, see below |
| gpt-oss after explicit load | loads in 35.7 s; **0.87 s** end-to-end through `oc-personal` |

MP3 is chosen over WAV for two reasons: it is 4.8x smaller (97 KB vs 465 KB,
comfortably under the 256 KB PSRAM cap), and `hal/AudioPlayer` already decodes
MP3, so the audio path is untouched. The WAV variant is IEEE float, which the
existing decoder does not handle.

## Root cause of the gpt-oss failure

`gpt-oss-120b-Q4_K_M` is registered by local path
(`/var/cache/models/lemonade/…`) — the extra-dir pattern the vault runbook warns
against. Both the chat-triggered autoload path **and** `POST /api/v1/pull`
resolve such models against the Hugging Face API, which 404s. Only an explicit
`POST /api/v1/load` bypasses the lookup, and that does not survive a `lemond`
restart.

Repairing the registration at the source requires either SSH to amd-halo (which
fails: `Host key verification failed`) or a ~60 GB catalog re-download. Neither
is available from the implementation environment, so the durable fix lives in
the proxy, which we do control.

## Design

### 1. Load-on-demand in `oc-personal-runner`

In `proxy.py`, when the backend returns a `model_load_error`, POST
`/api/v1/load` with the requested model name and retry the original request
once. On a second failure, surface the error as today.

This implements "it should load upon request", survives `lemond` restarts, and
with `max_models.llm = 2` gpt-oss then stays resident alongside Qwen3-Coder —
"loaded almost all the time".

**Known limitation, accepted:** this only fixes traffic through `oc-personal`
(Jarvis, morning-brief). Clients that hit Lemonade directly — Claude Code via
CCR — still get the 404 until the registration is repaired on amd-halo. Tracked
as a follow-up task below, not a blocker.

### 2. Repo reconcile

| File | Change |
|---|---|
| `tools/oc-personal-runner/.../config.py` | `BACKEND_URL` default `http://192.168.1.108:11434` → `http://amd-halo:13305`; document `OC_BACKEND_TOKEN` as the Lemonade API key |
| `src/config.h` | `kOcLocalModel` `gemma3n:e4b` → `gpt-oss-120b-Q4_K_M`; rewrite the Ollama comment block |
| `CLAUDE.md` | Ollama → Lemonade in both the project-state and external-endpoints sections |

No behavior change on the device from these — `OC_PROXY_FORCE_MODEL` already
overrides whatever model name the device sends. They stop the repo lying about
where inference happens.

### 3. TTS: a `lemonade` provider

Add `synthLemonade()` to `net/TtsClient`, alongside the existing
`synthOpenAi()` / `synthEleven()`:

```
POST http://<lemo_host>/api/v1/audio/speech
Authorization: Bearer <lemo_key>
Content-Type: application/json

{"model":"kokoro-v1","input":<text>,"voice":<lemo_voice>,"response_format":"mp3"}
```

This requires a **plain-HTTP path**. Both existing providers hardcode
`WiFiClientSecure` + `https://`; Lemonade is plain HTTP on the LAN, so
`synthLemonade()` uses a plain `WiFiClient`. `kokoro-v1` is hardcoded in
`config.h` rather than read from NVS, because Lemonade exposes exactly one TTS
model.

`http.end()` must fire on every exit path, per the project invariant.

### 4. NVS keys and the fallback chain

Three new keys, all within the 15-char limit, in namespace `jarvis`:

| Key | Purpose | Example |
|---|---|---|
| `lemo_host` | host:port of Lemonade | `192.168.1.118:13305` |
| `lemo_key` | Lemonade API key | 64-char, from 1Password `holdfast-lan` → "lemonade api key" |
| `lemo_voice` | kokoro voice | `af_sky` (verified working) |

Separate keys rather than reusing `tts_api_key` / `tts_voice_id`, because cloud
TTS is retained as the off-LAN fallback — a shared voice field would give one of
the two providers the wrong voice. Existing OpenAI/ElevenLabs credentials stay
untouched, so providers can be switched without reprovisioning the device.

All three are added to `ConfigSchema` so they appear in the captive portal.
`lemo_key` is write-only in the portal, matching `tts_api_key`.

Fallback chain, evaluated in `LLMModule::speak()`, first match wins:

| Condition | Action |
|---|---|
| `tier == LAN` and `lemo_host` + `lemo_key` both set | kokoro via Lemonade |
| `tier == TAILSCALE` or `HOTSPOT_ONLY` | cloud TTS, if `tts_api_key` is set |
| `tier == OFFLINE` | melotts, directly — no HTTP attempted |
| any of the above fails or is unconfigured | melotts |

`TAILSCALE` is grouped with `HOTSPOT_ONLY` rather than with `LAN` because
`lemo_host` is a LAN address (`192.168.1.118`) and is not routable from that
tier. This is deliberate: the CoreS3 cannot join a tailnet, so a `TAILSCALE`
tier reading never implies amd-halo is reachable.

The tier gate matters: the CoreS3 cannot join a tailnet and lobsterboy's
`tailscale serve` is tailnet-only (no Funnel), so `*.ts.net` is unreachable from
the device. Gating on tier avoids burning the full HTTP timeout budget on a host
that cannot be reached, consistent with how `personal_query` already
short-circuits to `kErrPersonalOffline`.

Any failure still ends in melotts speaking, so the "never return to IDLE
silently" invariant holds.

### 5. Thread the provider through `synthesize()` (bug fix)

`LLMModule::speak()` selects the provider source-aware — `tts_proact` for
`Proactive`, `tts_provider` for `Response` — to decide whether to attempt cloud
TTS. It then calls `TtsClient::synthesize(text)`, which **re-reads
`getTtsProvider()` internally**, ignoring the source.

Consequence today: a proactive push with `tts_proact=openai` and
`tts_provider=melotts` passes the gate, then hits `unsupported provider
"melotts"` inside `synthesize()`, returns empty, and falls back to melotts. The
proactive voice never applies — the PR #43 feature is silently inert for that
combination.

`synthesize()` gains an explicit provider parameter, and `speak()` passes the
one it already resolved. This is required by the fallback chain above (which
must be able to ask for a specific provider), and fixes the pre-existing bug as
a consequence.

### 6. Reply sanitizer

gpt-oss returns markdown — the verification prompt produced `The capital of
France is **Paris**.` — and a separate `reasoning_content` field. The firmware
reads only `choices[0].message.content`, so `reasoning_content` is harmless, but
the markdown reaches kokoro literally.

Strip `*`, `_`, and backticks from the reply before speaking, in `LLMClient`
where the reply is already truncated to `kOcMaxReplyChars`. Character-level
strip only — not a markdown parser.

### 7. Documentation

- Fix `192.168.1.61` → `192.168.1.118` in the 2ndBrain runbook.
- Record the gpt-oss autoload root cause and the proxy workaround.

## Out of scope

- **STT via Whisper.** Own spec. Requires CoreS3 mic capture, PSRAM WAV
  buffering, utterance endpointing, and multipart upload against a blocking
  single-threaded FSM. `Whisper-Large-v3-Turbo` is installed and ready.
- **Tailscale Funnel on amd-halo.** Would make the device's `TAILSCALE` tier
  real, but publishes an inference server to the internet behind only an API
  key. Separate decision.
- **Repairing the gpt-oss registration on amd-halo.** Follow-up; needs SSH
  access or a catalog re-download.

## Verification

| Claim | How |
|---|---|
| Firmware compiles | `pio run` |
| kokoro returns playable MP3 | done during design: 200, `audio/mpeg`, 97 KB |
| gpt-oss serves end-to-end | done during design: 0.87 s via `oc-personal` |
| Load-on-demand works | restart `lemond`, confirm the first Jarvis query succeeds without a manual load |
| Fallback chain | force each tier, confirm kokoro / cloud / melotts in the serial log |
| On-device audio | flash or OTA, speak a query, confirm kokoro voice — **requires hardware, user-run** |

The last row cannot be verified from the implementation environment. It will be
reported as unverified rather than claimed.

## Decisions taken

1. STT split into its own spec rather than bundled.
2. TTS goes direct to amd-halo; LLM stays behind `oc-personal`, because that
   service is not just a proxy — it is the 12-tool MCP agent loop for
   `personal_query`.
3. Cloud TTS retained as the off-LAN fallback, via dedicated `lemo_*` NVS keys.
4. `gpt-oss-120b-Q4_K_M` kept as the model, fixed rather than swapped.
5. The fix lives in the proxy; amd-halo re-registration is a follow-up.

# Hermes ↔ Lemonade ↔ Jarvis — session findings

Date: 2026-11-07
Status: findings record. Companion to `plans/hermes-control.md`,
`plans/hermes-mcp-spec.md`, `plans/hermes-mcp-plan.md`, `plans/jarvis-fleet-spec.md`.
Nothing here is implemented; all probes were read-only except one reversible
provider registration (since removed).

This captures what was *verified live* on `amd-halo` in the 2026-11-07 session —
especially the material not already in the design docs above: Lemonade's router
mechanics, the container networking wall, measured audio latency, and the
outcome of the "Hermes as a router candidate" experiment.

---

## 1. The question

Can Jarvis connect to and control Hermes agents? Yes (see `hermes-control.md`).
The follow-on question this session chased: where should the **model-selection**
(router) layer live, and can Hermes be one of its candidates?

## 2. Lemonade router / collection mechanics (verified against docs)

Source: <https://lemonade-server.ai/docs/api/router/> and
<https://lemonade-server.ai/docs/dev/router-policy/>.

- `collection.router` is a **virtual model** — `recipe: "collection.router"`, a
  sibling of `collection.omni`. Naming it in the OpenAI `model` field triggers
  routing. First-matching rule wins; unmatched → `default_model`.
- **No `/v1/route` endpoint and no `"auto"` model.**
- Register with `POST /v1/pull` (policy JSON). Test without registering via
  `POST /v1/routing/validate` (`policy`, `prompt`, `has_tools`, `has_images`,
  `metadata`) — returns the decision + full per-condition trace.
- Decision surfaces on the response: header `x-lemonade-route`, and body
  `x_lemonade_route` when the request sets `route_trace: true`.
- **Candidates** are members of `components`; `route_to` must be a candidate.
  **Deterministic** conditions (`keywords_any/all`, `regex`, `min_chars`/
  `max_chars`/`min_total_chars`, `has_tools`, `has_images`, `metadata`) evaluate
  locally. **Model-backed** (`semantic_similarity`, `classifier`, `llm`) may
  load a helper model.
- `routing.router` (LLM-as-router) is a shorthand that replaces rules entirely
  (expands to one `llm` classifier + one rule per candidate). Mutually
  exclusive with `routing.rules` / `routing.classifiers`.
- "Candidates should be chat-capable LLMs" — a router selects a **model** per
  request, not a modality and not an agent.

### Cloud/custom providers — arbitrary `base_url` is supported

`lemonade cloud install <provider>` accepts:

| flag | meaning |
|---|---|
| `--base-url URL` | OpenAI-compat base URL (**must include `/v1`**) |
| `--allow-insecure-http` | permit `http://` for this provider's key |
| `--auth-header-name` / `--auth-header-prefix` | custom auth header (default `Authorization`, `Bearer `) |
| `--wire-format {openai,anthropic}` | request/response shape |

Live evidence: `lemonade cloud list` shows `openrouter https://openrouter.ai/api/v1`,
and **local providers `gufo` / `gufoflash` are registered this way** (env files
`/etc/lemonade/conf.d/gufo.conf` with `LEMONADE_GUFO_API_KEY=local-no-auth`). So
pointing a candidate at a local OpenAI-compatible endpoint is a first-class,
already-used pattern.

## 3. Container networking wall (the blocker — verified)

**Lemonade runs in a podman container** `lemonade-server`
(`ghcr.io/lemonade-sdk/lemonade-server:latest`, image ~3 days old, version
**2026.45.3**) under **pasta networking** — *a separate network namespace*. The
host's own `lemond` 11.8.1 is only the fallback named in the unit description.

Consequences measured inside the container:

| Target from container | Result |
|---|---|
| `127.0.0.1:8642` (Hermes) | `000` — container's own loopback, nothing there |
| `192.168.1.118:13305` | `200` — **the container answering itself** (pasta shares the host IP) |
| `169.254.1.2:17707` (runner, bound `0.0.0.0`) | `404` → reached ✅ |
| `169.254.1.2:5000` / `:4096` (bound `0.0.0.0`) | `200` ✅ |
| `192.168.1.118:8642` (Hermes) | `000` |

**Rule: the container reaches any host service bound to `0.0.0.0` via
`169.254.1.2` (`host.containers.internal`), but not a loopback-only bind.**

Hermes binds **`API_SERVER_HOST=127.0.0.1`** (`~/.hermes/.env`;
`config.yaml platforms.api_server` wins over env). It is launched as a child of
`hermes-gateway.service`. So the container cannot reach Hermes at all →
`models_discovered: 0`.

This is a **networking wall, not a Lemonade limitation.**

## 4. The "Hermes as router candidate" experiment (done, then rolled back)

1. `lemonade cloud install hermes --base-url http://127.0.0.1:8642/v1 --allow-insecure-http` → registered ✅
2. `lemonade cloud auth hermes --api-key …` → `runtime_key_set: yes` ✅
3. `models_discovered` → **0** ❌
4. `POST /chat/completions model=hermes.hermes-agent` → **404 model_not_found** ❌

Root cause = §3. **Cleaned up:** `lemonade cloud uninstall hermes` +
`cloud clear hermes`; final provider list is `openrouter` only. No bind change,
no unit change, no restart.

**To make it work** Hermes would have to bind off loopback (`API_SERVER_HOST=0.0.0.0`
or a specific non-loopback address) — which exposes the Hermes API to the
LAN/tailnet behind its bearer key. Not free. And even then it is
Hermes-**as-a-chat-model** (one blocking turn), not run control.

### Security note to action

During step 1 an early attempt used the wrong CLI flag (`--key` instead of
`--api-key`) and the CLI **echoed the Hermes `API_SERVER_KEY` into its error
output**. → **Rotate `API_SERVER_KEY`** in `~/.hermes/.env`, and update every
consumer (the `hermes` API server, `cardputer-bridge` config, any registered
Lemonade provider using it).

## 5. Measured audio latency (live, warm unless noted)

| Stage | Warm | Cold (load) | Notes |
|---|---|---|---|
| STT — Whisper-Base | 45 ms | 0.96 s | correct transcript |
| STT — Whisper-Small | 107 ms | 1.02 s | correct transcript |
| STT — Whisper-Large-v3-Turbo | 454 ms | 3.63 s | installed, idle today |
| TTS — kokoro-v1 | 2.08 s | — | 36 KB MP3 for one short sentence |
| Brain — Hermes agent turn | 5–10 s | — | same model called direct: still ~10 s |

Endpoints: `POST /api/v1/audio/transcriptions` (needs `model=<canonical id>`,
e.g. `Whisper-Small`, not `whisper-small`), `POST /api/v1/audio/speech`
(`{model:"kokoro-v1", input, voice:"af_sky", response_format:"mp3"}`).

**Conclusion:** the audio path is *not* the bottleneck — STT is ~100 ms warm and
TTS ~2 s. The seconds live in the **brain**. The one real STT cost is
**cold-load** (keep the model resident).

Also observed: NPU-optimized variants exist in the catalog
(`whisper-base/small/large-turbo-onnx-npu`, `whisper-v3-turbo-FLM`) — the NPU
lever is best aimed at the *thinking* model, not the 45 ms transcriber.

## 6. What this implies for the design

- The clean composition is **nested**: Jarvis → Hermes (agent) →
  `user.VoiceRouter` (Lemonade `collection.router`) → small / big model. Router
  does model choice; Hermes keeps the lifecycle.
- Hermes-as-a-candidate is real but blocked by the bind, and flattens the agent.
- `hermes-mcp` (spec/plan in this repo) remains the only path to **run control**
  (sessions, approvals, SSE).
- Fast/slow split belongs in Lemonade as a declarative policy — deterministic
  rules are ~free; LLM-as-router adds a judge call.
- The **candidate pool is large**: 237 models; small chat models exist
  (`Bonsai-1.7B/4B`, `LFM2.5-1.2B-Instruct`, `Jan-v1-4B`, `K2-Horizon-0.9B`,
  `Gemma-3-4b`). Note many small models are *not* tool-calling, so the split is
  really "needs tools/context → coder; chat-only → small", not "short → small".

## 7. Live host facts (2026-11-07)

- `amd-halo`: Ryzen AI MAX+ 395 (Strix Halo), 32 threads, **125 GiB RAM**
  (82 free), Radeon 8060S iGPU w/ ROCm, load ~0.17 (idle).
- Services: `hermes-gateway.service` (API server `127.0.0.1:8642`),
  `lemonade-server.service` (container, `0.0.0.0:13305`),
  `cardputer-bridge.service` (`192.168.1.118:8765`), OpenCode (`0.0.0.0:4096`),
  `oc-personal-runner` (`0.0.0.0:17707`).
- Hermes Agent **v0.22.7 (2026.9.24)**, by Nous Research, at `~/.hermes/hermes-agent/`,
  entry point `~/.local/bin/hermes`.

## 8. Open / unverified

- Whether rebinding Hermes to `0.0.0.0` is acceptable (security trade) — owner decision.
- Whether Hermes should be the agent vs. a router candidate (direction open, §11
  of `hermes-control.md`).
- Host `lemond` 11.8.1 vs the container's 2026.45.3 — which serves the box day
  to day (container confirmed active; fallback described in the unit).

---

## 9. Audio-in chat models (`chat-transcription`) — tested 2026-11-08

Two things named "omni" exist, and they are not the same:

- **`LMX-Omni-*`** = `collection.omni` **bundles** (planner LLM + image model +
  ASR + TTS) orchestrated by the server's OmniRouter. Their ASR is a *tool*
  (client-side); they do **not** unify speech-in. Not useful for Jarvis.
- **`chat-transcription` models** = a single model that takes audio **inside the
  chat turn**. This is the real one-hop input.

**Wire shape (undocumented, verified working):** audio goes into
`POST /v1/chat/completions` as a content part, parallel to `image_url`:

```json
{"type":"input_audio","input_audio":{"data":"<base64 WAV>","format":"wav"}}
```

### `Qwen2.5-Omni-3B-GGUF` (llamacpp, GPU)

Pulled (4.7 GB). Cold first load **22.1 s**; warm **0.12–0.92 s**.

| Prompt | Warm | Output |
|---|---|---|
| "Transcribe verbatim" | 0.76 s | *"The weather is sunny and warm with a high of 82 degrees"* ← **hallucinated** |
| "Transcribe verbatim" (again) | 0.12 s | *"What is the weather like in Austin today"* ✓ |
| "What is the person asking for?" | 0.70 s | ✓ |
| "What did the person say?" | 0.92 s | *"I'm unable to access the internet…"* ← answered instead of transcribing |

Labels `chat, vision, chat-transcription` — **no `tool-calling`**. It can hear,
but cannot act, and it is inconsistent at transcription (it is a chat model that
can hear, not an ASR engine). Whisper remains more trustworthy for pure STT.

### `gemma4-it-e4b-FLM` (flm, **NPU**) — the genuine one-hop voice agent

Pulled (9.1 GB; the FLM `pull` is slow — tens of minutes). Runs on **npu**
(backend `:8002`), leaving the GPU free for the big models. Consistent — unlike
Qwen-Omni. Labels `audio, vision, reasoning, tool-calling, chat-transcription, chat`.

| Test | Latency | Result |
|---|---|---|
| audio → transcribe (cold) | 7.53 s | *"Set a timer for 10 minutes."* ✓ |
| audio → transcribe (warm) | 2.41 s | same ✓ (consistent) |
| audio → intent | 2.57 s | *"wants to set a timer for ten minutes"* ✓ |
| **audio → tool call** | **2.73 s** | `set_timer({"minutes":10})` ✓ |

**This is the finding that changes the design.** One request: audio in →
understanding → **structured tool call** out, in ~2.7 s, on the NPU.

### Comparison

| Path | Warm latency | Audio-in | Tools | Device |
|---|---|---|---|---|
| Whisper + coder agent (today) | ~5–10 s | separate call | yes | GPU |
| `Qwen2.5-Omni-3B` | 0.1–0.9 s | one hop | **no** | GPU |
| `gemma4-it-e4b-FLM` | 2.4–2.7 s | one hop | **yes** | **NPU** |
| Whisper (pure ASR) | ~0.1 s | — | n/a | CPU |

**Design implication:** the fast voice path can be a single
`chat-transcription` + `tool-calling` model (hears → decides → calls a tool) on
the NPU, with the GPU left for the heavy agent (coder). Whisper stays the pure
ASR fallback; `LMX-Omni` bundles are not the answer.

**Caveats:** e4b is small (shallow understanding); ~2.5 s is not sub-second;
the FLM pull is slow; NPU needs `/dev/accel0` (present) + `xrt-smi`.
Disk after both pulls: **72 GB free, 97 % used**.

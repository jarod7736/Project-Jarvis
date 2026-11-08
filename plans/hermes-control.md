# Hermes control from Jarvis — feasibility and design

Date: 2026-11-07
Status: design only. Nothing here is implemented; no code was written or deployed.
Question answered: **can Jarvis connect to and control Hermes agents?** Yes.

This document records what was verified against the live host, the integration
options, the recommended design, and the constraints that decide whether it is
worth building.

---

## 1. Answer in one line

Yes. Hermes runs an OpenAI-compatible API server on `amd-halo`, and Jarvis
already speaks that shape through `oc-personal-runner`. Full run control
(start / watch / approve / stop) is reachable without any firmware change, by
adding one stdio MCP child to the runner.

---

## 2. What "Hermes" is here

**Hermes Agent** — an AI assistant with tool-calling, **by Nous Research** (not
the Nous Hermes *model* family; this is the agent that consumes models). Version
on `amd-halo`: **v0.22.7 (2026.9.24)**. Installed at `~/.hermes/hermes-agent/`,
entry point `~/.local/bin/hermes`. It keeps sessions, runs its own agent loop,
has skills (`~/.hermes/skills/`, per-project `.hermes/skills/`), supports MCP as
a *client* (`hermes mcp`), and runs a messaging gateway.

**Correction (2026-11-07).** Earlier drafts of this repo's docs framed Hermes as
a headless run API. It is **voice-native**. Verified on the install:

- STT: `config.yaml` `stt:` block — `enabled: true`, `local.model: base`,
  `openai.model: whisper-1`. Built-in providers
  (`agent/transcription_registry.py`): local, local_command, groq, openai,
  mistral, xai, elevenlabs, deepinfra.
- TTS (`agent/tts_registry.py`): edge, elevenlabs, openai, minimax, xai,
  mistral, gemini, neutts, kittentts, piper, deepinfra.
- Voice turn handling, per-chat `/voice on|tts|off`, voice-channel join, and
  auto-TTS on voice input: `gateway/run_voice.py`, `gateway/platforms/base.py`.
  Wake-word/mention pattern compilation: `gateway/platforms/helpers.py`.
- Reported by the user, not independently traced: wake word "Hey Hermes",
  push-to-talk (Ctrl+B) in the TUI, Discord voice channels, Telegram voice
  notes, self-hosted realtime gateways (hermes-live-voice / Apollo).

**Surfaces it already exposes** (from `hermes --help`):

| Command | What it is |
|---|---|
| `hermes serve` | headless backend server (desktop app / remote backends) |
| `hermes acp` | Agent Client Protocol server (VS Code, Zed, JetBrains) |
| `hermes proxy` | local OpenAI-compatible proxy to OAuth providers |
| `hermes peer dm/run/status/stop` | bot-to-bot DMs and **async remote run control** across Hermes gateways |
| `hermes -z/--oneshot` | one prompt, final text only, approvals bypassed — scriptable |
| `hermes send` | send a message to a configured platform (scripts/cron/CI) |
| `hermes gateway` | messaging platforms: Telegram, Discord, WhatsApp, Weixin, Slack, … |
| `hermes mcp` | manage MCP servers (Hermes as MCP **client**) |
| `hermes approvals` | mine approval history into allowlist proposals |

Hermes is named in `plans/todo.md` §D as duplicating Google (Calendar/Gmail)
code — a full agent with its own tool surface.

## 3. Verified state on amd-halo (live check, 2026-11-07)

Services running (`systemctl`, `ss -tlnp`):

| Service | Bind | Notes |
|---|---|---|
| `hermes-gateway.service` | — | "Hermes Agent Gateway — Messaging Platform Integration" |
| Hermes API server | `127.0.0.1:8642` | pid owned by `hermes`; OpenAI-compatible |
| `cardputer-bridge.service` | `192.168.1.118:8765` | card/SSE bridge to Hermes + OpenCode |
| OpenCode | `0.0.0.0:4096` | separate agent, same bridge |
| `oc-personal-runner` | `0.0.0.0:17707` | what Jarvis talks to |
| Lemonade `coder` | `0.0.0.0:13305` | local model backend |

Host access: LAN `192.168.1.118`, tailnet `100.120.121.35`; SSH alias `amd-halo`
works (LAN and tailnet). `sudo` on `amd-halo` **requires a password** (not
`NOPASSWD`) — relevant to deployment, see §8.

## 4. The Hermes API surface

Base `http://127.0.0.1:8642`, every route needs `Authorization: Bearer
<API_SERVER_KEY>`. Confirmed against `~/.hermes/hermes-agent/gateway/platforms/
api_server.py` (OpenAI-compatible frontend, `X-Hermes-Session-Id` continuity
header) and the working bridge adapter `~/apps/cardputer-bridge/cardputer_bridge/
adapters/hermes.py`.

| Method | Path | Body / result |
|---|---|---|
| GET | `/v1/health` | 200 when healthy |
| GET | `/api/sessions?limit=N` | `{object:"list", data:[{id,title,preview,last_active,started_at}]}` |
| POST | `/v1/runs` | `{input, session_id, provider, model}` → `{run_id}`; returns at once |
| GET | `/v1/runs/{run_id}/events` | SSE; `Last-Event-ID: <seq>` resumes |
| POST | `/v1/runs/{run_id}/approval` | `{request_id, execution_generation, choice: once\|always\|deny}` |
| POST | `/v1/runs/{run_id}/stop` | terminal |

SSE events: `message.delta{delta}`, `tool.started{tool,preview}`,
`tool.completed{tool,error?}`, `approval.request{prompt_id,
execution_generation, choices, description, command}`, `approval.settled`,
`run.completed`, `run.failed{error}`, `run.cancelled`,
`run.interrupted{turn_exit_reason}`. Informational, ignore: `hermes.status`,
`run.queued`, `run.started`, `reasoning`. Each payload carries `seq`.

Also present: `/v1/chat/completions` and `/v1/responses` (OpenAI-compat).

Notes: **Hermes has no create-session route** — `POST /v1/runs` accepts any
`session_id` string. Sessions are discovered, not created.

## 5. Why Jarvis can already reach it

`oc-personal-runner` (`tools/oc-personal-runner/`) is an OpenAI-compatible front
on `:17707`. `model="oc-personal"` runs a multi-MCP agent loop (currently 12
stdlib children: 6 brain + 6 google, spawned per `OC_MCP_SERVERS`, see
`src/oc_personal/agent.py`). Every other model name is proxied to Lemonade. The
CoreS3 firmware sends plain `POST /v1/chat/completions`; two intents route there
(`personal_query`, `local_llm`).

Consequences:

- The runner already hosts arbitrary stdio MCP children — that is the extension
  point. Adding Hermes is a config entry plus a new package, not a new subsystem.
- Hermes binds `127.0.0.1`, so anything reaching it must run on `amd-halo`. The
  runner and its children already do.
- No firmware change is needed for any option below except where noted.

## 6. Integration options

**Native — no build at all** (these answer "full voice control" on their own):

| # | Route | Gets you |
|---|---|---|
| 0 | Talk to Hermes directly — TUI voice (`/voice on`), Discord voice channels, Telegram voice notes, Discord/Telegram text | Full native voice control of Hermes; **Jarvis not involved** |
| 0b | `hermes peer run/status/stop <peer>` | async remote run control across Hermes gateways — already built |
| 0c | `hermes -z "<prompt>"` | one-shot scripted start; final text only |

**Via Jarvis — the actual question:**

| # | Route | Gets you | Firmware | Effort |
|---|---|---|---|---|
| 1 | Jarvis model/intent → Hermes `/v1/chat/completions` | Talk to Hermes; Hermes drives its own runs | config only | Low |
| 2 | `hermes-mcp` stdio child on `oc-personal-runner` | Full lifecycle: prompt, watch, approve/deny, stop | none | Medium |
| 2b | Jarvis as a Hermes gateway platform (new transport + Hermes's own STT/TTS) | Hermes drives the device end-to-end | firmware + platform work | High |
| 3 | Jarvis → `cardputer-bridge` (`:8765`) directly | Everything, in a card/SSE shape built for the Cardputer screen | none | Medium–high, awkward for voice |
| 4 | Reuse `cardputer-bridge` as the backend, wrapped as MCP | Option 2's tools over the bridge instead of Hermes directly | none | Medium |

Option 2's run-control shape is largely what `hermes peer run/status/stop`
already provides for a peer Hermes — if both ends can run Hermes, prefer `peer`
over hand-building. Option 3/4 reuses working code but the bridge's model is a
card feed for a screen device with its own device token: overkill for voice.
Option 2 mirrors the brain/google pattern exactly and keeps Hermes credentials
server-side. Option 2b is the literal "the device is a Hermes endpoint" answer.

## 7. Recommended design — `hermes-mcp`

A new stdio MCP server `tools/hermes-mcp/` (src-layout, hatchling, `mcp<2`,
`httpx`), spawned by `oc-personal-runner` as a third `OC_MCP_SERVERS` entry.

Because Hermes runs are long and asynchronous while a Jarvis turn is short and
blocking (12 s HTTP timeout, `OC_MAX_TURNS=4`), the server must be
**non-blocking**: it keeps a process-wide run registry and, per run, one
background task that consumes the SSE stream and tracks transcript, state, and
any pending approval. Tools return immediately; the assistant reports progress
on a later utterance.

### Tool surface (5 tools → 17 total on the runner)

| Tool | Args | Returns |
|---|---|---|
| `hermes_sessions` | `limit=10` | JSON list of recent sessions |
| `hermes_run` | `prompt`, `session_id?` | `{run_id, session_id, state}` — non-blocking |
| `hermes_status` | `run_id?`, `session_id?` | `{state, text, pending_approval?, error}` |
| `hermes_approve` | `request_id`, `choice=once\|always\|deny` | JSON status; requires explicit `request_id` |
| `hermes_stop` | `run_id?`, `session_id?` | JSON status |

Config: `HERMES_API_URL` (default `http://127.0.0.1:8642`), `HERMES_API_KEY`
(required), `HERMES_PROVIDER` (default `custom:amd-halo`), `HERMES_MODEL`
(default `coder`).

The runner's `SYSTEM_PROMPT` gains a short Hermes block and routing rules:
route there only when Hermes is named; treat it as async (acknowledge, don't
poll); never approve without an explicit spoken yes.

## 8. Deployment (for whoever builds it later)

1. Add the package under `tools/hermes-mcp/` and `pip install -e .` into a
   `.venv` on `amd-halo` (no systemd unit — it is a stdio child).
2. Put `HERMES_API_KEY` in `~/.config/oc-personal/secrets.env` (mode 600,
   user-owned; already `EnvironmentFile`-loaded by the runner, and the MCP SDK
   merges the parent env into the child — so the key need not appear in the unit).
3. Add a `hermes` entry to `OC_MCP_SERVERS` in
   `/etc/systemd/system/oc-personal.service.d/override.conf`. **This needs
   `sudo`** — it is a root-owned drop-in and `sudo` is password-gated on
   `amd-halo`. Command path:
   `/home/jarod7736/projects/Project-Jarvis/tools/hermes-mcp/.venv/bin/python`,
   args `-m hermes_mcp.server`, `stderr=/tmp/hermes-mcp.err`.
4. `sudo systemctl daemon-reload && sudo systemctl restart oc-personal` (also
   `sudo`). Verify `OC /healthz` reports **17 tools** and includes the `hermes`
   server name.
5. Smoke test the server standalone first (no sudo): with the vault of env vars
   set, call `health()` and `list_sessions(3)`, then a benign
   `POST /v1/runs` + status + stop.

Sync note: the source of truth is this repo on the workstation; `amd-halo` has
its own clean clone at `~/projects/Project-Jarvis` (both were at `32c64cd`).
Ship changes by commit+push+pull or `rsync`.

## 9. Constraints and risks

- **Async vs voice mismatch — only when Jarvis is the voice front-end.** Hermes
  is itself voice-native, so conversational voice control of Hermes already
  exists through its own surfaces. Multi-minute runs feel clunky only when a
  seconds-long Jarvis turn has to drive them. If the device becomes a Hermes
  platform (option 2b) the mismatch moves into Hermes, which already handles
  long/background turns.
- **Voice approval is the dangerous part.** The prompt is explicit, but the
  model decides, and ASR is the known weak link (`plans/todo.md` §C: "write me a
  haiku" → "write me a high koo"). Any approve path should treat "yes" as
  low-confidence and prefer deny on ambiguity; never auto-approve.
- **Latency budget.** Five more tools lengthen the agent prefix. It is static
  (cache-friendly) but adds prefill on a cold cache — the one case already over
  the 12 s window.
- **`127.0.0.1` bind.** Only same-host processes can reach Hermes. Fine for the
  runner; a blocker for any device-direct design.
- **Credential handling.** `API_SERVER_KEY` must never reach the firmware. The
  MCP-child design keeps it on `amd-halo`.

## 10. Not verified / open questions

- Upstream identity of the Hermes agent (vendor, open-source repo, licence) —
  the external research lane failed (`ECONNREFUSED`); only the local install was
  inspected. `hermes-agent` is the local package name; `hermes gateway run`
  runs the gateway.
- Exact `approval.request` payload across all tool types (only the bridge's
  mapping was read, not live-traced).
- Whether `POST /v1/runs` needs `provider`/`model` always, or has server
  defaults.
- Whether the run registry should survive a runner restart (it does not, in the
  sketched design — in-memory only).

## 11. Direction

Reopened 2026-11-07 after the voice-native correction. The earlier choice — full
lifecycle over the `hermes-mcp` route (option 2) — assumed Hermes was headless.
It is not, and native options (0/0b/0c) already deliver voice control and remote
run control with no build. Which route Jarvis should take — native platform,
remote trigger, or none — is an open decision.

# Spec — `hermes-mcp`

Technical specification for a stdio MCP server that lets Jarvis connect to and
control Hermes agents. Scope is the server's contract only: interfaces, data,
and behaviour. Build steps live in `plans/hermes-mcp-plan.md`; the decision and
rationale live in `plans/hermes-control.md`.

Date: 2026-11-07
Status: spec. Not implemented.
Depends on: nothing in this repo had to change to write this.

---

## 1. Purpose

Expose the Hermes agent (running on `amd-halo`) as MCP tools so Jarvis's
personal-mode agent loop can start a run, watch it, answer its approval
requests, and stop it — over voice.

**Premise corrected (2026-11-07).** Hermes is *not* a headless run API: it is
voice-native (STT/TTS, wake word, voice modes) and already exposes ACP, an
OpenAI-compatible proxy, and cross-machine `peer run/status/stop`. This spec
deliberately keeps Jarvis as a *separate* voice endpoint that drives Hermes
remotely; the native alternatives are enumerated in `plans/hermes-control.md`
§6. Which route is wanted is an open decision (see §14).

## 2. Where it sits

```
CoreS3 (Jarvis)  --POST /v1/chat/completions-->  oc-personal-runner (:17707)
                                                     │ agent loop, model=coder
                                                     ├─ stdio: brain-mcp     (6 tools)
                                                     ├─ stdio: google-mcp    (6 tools)
                                                     └─ stdio: hermes-mcp    (5 tools)  ← this
                                                                 │ httpx
                                                                 ▼
                                                     Hermes API server (:8642, 127.0.0.1)
```

- `hermes-mcp` is a **stdio child of `oc-personal-runner`**, spawned once at
  runner startup and kept alive for the runner's lifetime (same lifecycle as
  `brain-mcp` / `google-mcp` — see `tools/oc-personal-runner/src/oc_personal/
  agent.py`, `BrainAgent.lifecycle`).
- It is the **only** component that holds `HERMES_API_KEY`. The key never
  reaches the firmware.
- It has **no systemd unit** and does not listen on a socket.

## 3. Upstream contract (Hermes API) — the dependency

Base URL default `http://127.0.0.1:8642`. Every request sends
`Authorization: Bearer <HERMES_API_KEY>`. Verified against the live service and
`gateway/platforms/api_server.py`.

| Method | Path | Request | Response |
|---|---|---|---|
| GET | `/v1/health` | — | 200 when healthy |
| GET | `/api/sessions` | `?limit=N` | `{object:"list", data:[{id,title,preview,last_active,started_at}]}` |
| POST | `/v1/runs` | `{input, session_id, provider, model}` | `{run_id}`; returns immediately |
| GET | `/v1/runs/{run_id}/events` | header `Last-Event-ID: <seq>` (optional) | SSE stream |
| POST | `/v1/runs/{run_id}/approval` | `{request_id, execution_generation, choice}` | 2xx; 404/409/410 = resolved/expired |
| POST | `/v1/runs/{run_id}/stop` | — | 2xx; 404 = gone |

**Premises the server relies on:**

- P1. Hermes has **no create-session route**; `/v1/runs` accepts any
  `session_id` string.
- P2. `/v1/runs` returns before the run finishes; progress arrives only on the
  events stream.
- P3. Each SSE payload carries `seq`, so a dropped stream resumes with
  `Last-Event-ID`.

### 3.1 SSE frame format

- Line-based. A line beginning `:` is a keepalive comment — ignore.
- `event: <name>` sets the event name for the frame.
- `data: <json>` accumulates; multiple `data:` lines join with `\n`.
- A blank line terminates a frame. The JSON payload also carries an `event`
  field; resolve the name as `data.get("event") or <event: name>`.
- Each frame's `seq` updates the resume cursor.

### 3.2 Event catalogue

| Event | Fields used | Effect on state |
|---|---|---|
| `message.delta` | `delta` | append to transcript |
| `tool.started` | `tool`, `preview` | log step |
| `tool.completed` | `tool`, `error?` | log step; `error` = failure |
| `approval.request` | `prompt_id`, `execution_generation`, `choices`, `description`, `command` | set `pending_approval`; state → `waiting` |
| `approval.settled` | `prompt_id` | clear `pending_approval` |
| `run.completed` | — | state → `done` (terminal) |
| `run.cancelled` | — | state → `cancelled` (terminal) |
| `run.failed` | `error` | state → `failed` (terminal), record `error` |
| `run.interrupted` | `turn_exit_reason` | state → `failed` (terminal) |
| `hermes.status`, `run.queued`, `run.started`, `reasoning` | — | ignored |

### 3.3 Other Hermes surfaces (not used by this spec)

Listed so the design space is explicit — several already provide what this spec
builds. See `plans/hermes-control.md` §6.

| Surface | Command | Use |
|---|---|---|
| Agent Client Protocol server | `hermes acp` | editor integration; a programmatic agent channel |
| OpenAI-compatible proxy | `hermes proxy` | point external apps at Hermes with any bearer token |
| Cross-machine run control | `hermes peer run/status/stop` | async start + status + stop of runs on another Hermes gateway |
| Headless backend | `hermes serve` | powers the desktop app / remote backends |
| Scripted one-shot | `hermes -z "<prompt>"` | single prompt, final text only, approvals bypassed |
| Messaging gateway | `hermes gateway` | Telegram, Discord (incl. voice channels), WhatsApp, Slack, … |

## 4. Configuration contract

All from environment; fail fast only on a missing key at first use.

| Var | Default | Meaning |
|---|---|---|
| `HERMES_API_URL` | `http://127.0.0.1:8642` | Hermes API base |
| `HERMES_API_KEY` | `""` | Bearer token; required in practice |
| `HERMES_PROVIDER` | `custom:amd-halo` | passed as `provider` on `/v1/runs` |
| `HERMES_MODEL` | `coder` | passed as `model` on `/v1/runs` |
| `HERMES_REQUEST_TIMEOUT` | `30` | seconds, non-stream requests |
| `HERMES_MAX_TRANSCRIPT_CHARS` | `4000` | cap on retained transcript tail |

Secrets come via the runner's `EnvironmentFile`
(`~/.config/oc-personal/secrets.env`); the MCP SDK merges the parent env into
the child, so the key need not be duplicated into `OC_MCP_SERVERS`.

## 5. Internal data model

```
RunState:
  run_id: str
  session_id: str
  state: "queued" | "running" | "waiting" | "done" | "failed" | "cancelled" | "lost"
  transcript: str            # accumulated message.delta, capped to MAX_TRANSCRIPT_CHARS (tail kept)
  steps: list[str]           # recent tool.started / tool.completed one-liners (bounded, e.g. 20)
  pending_approval: Approval | None
  error: str | None
  updated_at: float

Approval:
  request_id: str            # = prompt_id
  execution_generation: int
  choices: list[str]         # subset of ["once","always","deny"]
  description: str
```

`RunRegistry` (process singleton):
- `start(state) -> None` — register and launch the background consumer task.
- `get(run_id) -> RunState | None`
- `latest_for_session(session_id) -> RunState | None`
- `latest() -> RunState | None`
- `find_by_request_id(request_id) -> RunState | None`
- Bounded: keep the most recent N (e.g. 20) runs; evict oldest terminal first.
- All mutations guarded by one `asyncio.Lock`.

## 6. Tool contract

Five tools. All are `async def`; every tool returns a JSON string. On failure
they return `{"error": "<message>"}` rather than raising, so the voice model can
speak a plain failure (the runner wraps raised exceptions as `"Tool X raised: ..."`).

### 6.1 `hermes_sessions(limit: int = 10) -> str`
Returns `{"sessions": [{"id","title","updated","state"}]}`. `state` is unknown
(always `"idle"`) — Hermes session listing carries no live run state; live state
comes from `hermes_status`.

### 6.2 `hermes_run(prompt: str, session_id: str | None = None) -> str`
Starts a run and returns at once. If `session_id` is omitted, generate one
(`jarvis-<8 hex>`). Returns
`{"run_id": str, "session_id": str, "state": "running"}`.
Docstring intent: **start only; do not wait or poll.**

### 6.3 `hermes_status(run_id: str | None = None, session_id: str | None = None) -> str`
With neither id, use the most recent run. Returns
```
{"run_id","session_id","state","text","steps":[...],"pending_approval"|null,"error"|null}
```
`text` is the transcript tail (bounded) so the assistant can speak progress.
`pending_approval` when present carries `request_id`, `description`, `choices`.

### 6.4 `hermes_approve(request_id: str, choice: str = "once") -> str`
- `choice` ∈ `{once, always, deny}`; anything else → `{"error":"invalid choice ..."}`.
- Look up `request_id` in the registry; if unknown/expired →
  `{"error":"unknown or expired approval <id>"}`.
- POST `/v1/runs/{run_id}/approval`; clear the registry's `pending_approval` on
  2xx; on 404/409/410 return `{"status":"already_resolved"}`, else `{"error":...}`.
- Returns `{"status":"ok","run_id":...,"choice":...}` on success.
- Docstring intent: **the assistant must confirm aloud with the user first; never
  approve unprompted; a `request_id` must come from a prior `hermes_status`.**

### 6.5 `hermes_stop(run_id: str | None = None, session_id: str | None = None) -> str`
Resolves the run like `hermes_status`; POST `/v1/runs/{run_id}/stop`. Returns
`{"status":"stopped","run_id":...}` or `{"error":...}`. Idempotent-ish: 404 →
`{"status":"not_running"}`.

## 7. Background consumer

Per run, one `asyncio.Task`:

1. `stream_events(run_id)`; on each frame update `RunState`, tracking `seq`.
2. On transient httpx error / read timeout: reconnect with `Last-Event-ID`,
   backoff 1/2/4/8 s capped at 15 s, bounded retries (e.g. 5).
3. On 404/410: state → `lost`, stop.
4. On a terminal event: finalize and return.
5. Never let an exception escape the task silently — record it as `error`.

Tasks live on the FastMCP server's event loop and survive across tool calls.

## 8. Concurrency and lifecycle

- Registry and per-run tasks are in-memory; **state is lost on runner restart**
  (accepted — Hermes remains the record of truth; sessions are re-listed).
- One run per session at a time; `hermes_run` on a session with an active run
  returns `{"error":"a run is already active for <session>"}`.
- The MCP server serializes as needed with its own lock (Jarvis produces one
  voice query at a time anyway).

## 9. Error handling and degradation

| Condition | Behaviour |
|---|---|
| `HERMES_API_KEY` empty | tools return `{"error":"HERMES_API_KEY not set"}` |
| Hermes unreachable | `{"error":"hermes unreachable: <reason>"}` |
| `/v1/runs` non-2xx | `{"error":"start failed: HTTP <n>"}` |
| Approval already resolved | `{"status":"already_resolved"}` |
| Bad `choice` | `{"error":"invalid choice <x>; use once|always|deny"}` |
| Stream lost | state `lost`; `hermes_status` reports it; no crash |

## 10. Security and safety

- The key is read from env only; never logged, never echoed in a tool result.
- **Approvals are never automatic.** `hermes_approve` requires an explicit
  `request_id` obtained from `hermes_status`. The tool docstring instructs the
  assistant to confirm aloud first; the runner's `SYSTEM_PROMPT` reinforces it.
  Rationale: ASR mistranscription (`plans/todo.md` §C) makes a spoken "yes"
  low-confidence for destructive actions.
- No network listener, no write path beyond the Hermes API.

## 11. Runner integration contract

1. New `OC_MCP_SERVERS` entry `"hermes"`:
   `command=<repo>/tools/hermes-mcp/.venv/bin/python`,
   `args=["-m","hermes_mcp.server"]`, `env={HERMES_API_URL, HERMES_PROVIDER,
   HERMES_MODEL}` (key via inherited env), `stderr="/tmp/hermes-mcp.err"`.
2. `SYSTEM_PROMPT` (`tools/oc-personal-runner/src/oc_personal/config.py`) gains a
   `HERMES AGENT` tool block and three routing rules: name-only routing, treat as
   async (acknowledge, don't poll), approval requires an explicit yes.
3. Stale tool count in that prompt (`"ten tools"`) is corrected to the true
   total once the entry is live (12 → 17).
4. Docs to update on ship: `CLAUDE.md`, `README.md`, `AGENTS.md`, `plans/todo.md`.

## 12. Non-goals

- No firmware change.
- No OpenCode control (the bridge covers it separately).
- No exposure of Hermes beyond `127.0.0.1`; no tailnet/LAN re-bind.
- No persistence of run state across runner restarts.
- No auto-approval policy engine.

## 13. Verification / acceptance criteria

1. `deploy.sh test` succeeds: `health()` 200 and `list_sessions(3)` returns data.
2. Standalone: `hermes_run` a benign prompt → `hermes_status` shows progress →
   `hermes_stop` returns `stopped`.
3. `OC /healthz` after wiring reports **17 tools** and server names
   `[brain, google, hermes]`.
4. A spoken "ask Hermes to …" utterance creates a run (confirmed by a follow-up
   status utterance), and a pending approval is surfaced, not auto-approved.
5. Runner log shows no `hermes-mcp` startup error; `/tmp/hermes-mcp.err` is clean.

## 14. Open questions

- Upstream identity/licence of `hermes-agent` (local install only was inspected).
- Whether `/v1/runs` has server defaults for `provider`/`model`.
- Registry eviction count and whether a run should outlive the voice session that
  started it (currently yes, until evicted/evicted by count).

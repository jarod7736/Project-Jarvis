# Plan — implement `hermes-mcp`

Implementation plan for the server specified in `plans/hermes-mcp-spec.md`
(decision in `plans/hermes-control.md`). Nothing here is done yet. Each step is
coarse enough to be one commit; verification gates are called out per step.

> **Direction caveat (2026-11-07).** Hermes is voice-native and already ships
> `peer run/status/stop` (async remote run control), `acp`, `serve`, and `-z`.
> This plan assumes the chosen route is **option 2 (Jarvis as a remote trigger
> via `hermes-mcp`)**. If the chosen route is the native platform (option 2b) or
> "talk to Hermes directly" (option 0), most of these steps are moot. Confirm
> the route before executing.

## Steps

- [ ] 1. Scaffold the `tools/hermes-mcp/` package
- [ ] 2. Implement `client.py` — Hermes HTTP + SSE
- [ ] 3. Implement `runs.py` — registry and background consumer
- [ ] 4. Implement `server.py` — the five MCP tools
- [ ] 5. Add `deploy.sh` and `README.md`
- [ ] 6. Verify standalone against live Hermes (no sudo, on amd-halo)
- [ ] 7. Sync to amd-halo and build the venv
- [ ] 8. Update the runner prompt and docs
- [ ] 9. Wire `OC_MCP_SERVERS` and restart (needs sudo)
- [ ] 10. End-to-end verification
- [ ] 11. Close out and commit

## Detail

### 1. Scaffold the package
Create `tools/hermes-mcp/` mirroring `tools/google-mcp/`: `pyproject.toml`
(name `hermes-mcp`, hatchling, `requires-python >=3.11`, deps
`mcp>=1.0.0,<2` + `httpx>=0.27`, script `hermes-mcp = "hermes_mcp.server:main"`),
`src/hermes_mcp/__init__.py`, `.gitignore` (`.venv/`, `*.egg-info/`), and
`src/hermes_mcp/config.py` with the env vars from spec §4.

**Gate:** `python -c "import hermes_mcp"` under a throwaway venv.

### 2. `client.py` — Hermes HTTP + SSE
Async `httpx.AsyncClient` wrapper: `health`, `list_sessions`, `start_run`,
`stream_events` (async generator of `(event, data)`), `approve`, `stop`. Port
the SSE parser and reconnect logic from the working bridge adapter
`~/apps/cardputer-bridge/cardputer_bridge/adapters/hermes.py` on `amd-halo`
(do not depend on it at runtime — copy the parsing, cite it in a comment).

**Gate:** a scratch script lists sessions and streams a benign run's events.

### 3. `runs.py` — registry and background consumer
`RunState` + `Approval` dataclasses (spec §5); `RunRegistry` singleton with the
lookups in spec §5 and the per-run consumer task in spec §7 (backoff, resume,
terminal handling, error capture). `asyncio.Lock` around mutations; bounded to
~20 runs.

**Gate:** unit-ish check — feed a fake event stream, assert state transitions.

### 4. `server.py` — the five MCP tools
`FastMCP("hermes")`; the tools in spec §6, all `async def`, all returning JSON
strings, all with the docstrings the voice model reads. `main()` = `mcp.run()`.
Invalid `choice` rejected; approvals never automatic.

**Gate:** `mcp dev` (or the inspector) shows 5 tools with correct schemas.

### 5. `deploy.sh` and `README.md`
Mirror `tools/google-mcp/deploy.sh`: `survey` (default), `install` (venv +
`pip install -e .` + import smoke test), `test` (env-driven `health()` and
`list_sessions(3)`). No systemd unit. `README.md` per spec §1/§4/§10.

**Gate:** `./deploy.sh survey` runs clean.

### 6. Verify standalone against live Hermes (no sudo, on amd-halo)
`HERMES_API_KEY` already sits in `~/.hermes/.env` / the bridge config; export it
for the test only, never echo it. Run `deploy.sh test` on `amd-halo`; then a
benign `hermes_run` → `hermes_status` → `hermes_stop` cycle via the MCP server.

**Gate:** spec §13.1 and §13.2 pass. **Stop here if they don't.**

### 7. Sync to amd-halo and build the venv
`rsync -a tools/hermes-mcp/ amd-halo:~/projects/Project-Jarvis/tools/hermes-mcp/`,
then build the venv on `amd-halo`. (The two checkouts are separate clones; both
were at `32c64cd`. A commit+push+pull works too, once the source is committed.)

**Gate:** `.venv/bin/python -m hermes_mcp.server` starts and lists 5 tools.

### 8. Update the runner prompt and docs
- `tools/oc-personal-runner/src/oc_personal/config.py`: add the `HERMES AGENT`
  tool block and routing rules (name-only routing; async — acknowledge, don't
  poll; approval needs an explicit spoken yes); correct the stale tool count.
- `CLAUDE.md`, `README.md`, `AGENTS.md`: tool count 12 → 17 and the new server.
- `plans/todo.md` §D: note the shared-Google-code item still stands.

**Gate:** prompt diff reviewed; no other tool descriptions disturbed.

### 9. Wire `OC_MCP_SERVERS` and restart (needs sudo)
Add the `"hermes"` entry to
`/etc/systemd/system/oc-personal.service.d/override.conf` (root-owned; `sudo`
is password-gated on `amd-halo`). Run:
```
sudo systemctl daemon-reload && sudo systemctl restart oc-personal
```
If `HERMES_API_KEY` is not already in `~/.config/oc-personal/secrets.env`, add it
there first (user-owned, no sudo).

**Gate:** `OC /healthz` reports 17 tools, servers `[brain, google, hermes]`.

### 10. End-to-end verification
Spec §13.3–13.5: healthz tool count; a spoken "ask Hermes to …" utterance starts
a run; a follow-up status utterance reports it; a pending approval is surfaced
and not auto-approved; `/tmp/hermes-mcp.err` clean.

**Gate:** all of spec §13 green.

### 11. Close out and commit
Commit the package + runner prompt + docs together (one coherent change).
`git log --oneline` for the message style (conventional commits).

## Sequencing and dependencies

- Steps 1–5 are one lane, no host access.
- Step 6 needs a key; steps 7 and 9 need `amd-halo`; step 9 additionally needs
  the user for `sudo`.
- Step 8 is independent of 6–7 and can run in parallel with them.
- Only step 9 changes live behaviour. Everything before it is reversible without
  touching the running stack.

## Risks

- **Cold-cache latency.** Five more tools lengthen the agent prefix; static, so
  cache-friendly, but a cold prompt cache is already the one case over the 12 s
  window (`plans/todo.md` §A).
- **Async/voice mismatch.** Runs take minutes; a Jarvis turn is seconds. Control
  is "start, then report later", not "do it and tell me".
- **ASR on approvals.** The safety rule is prompt-level; the model still
  decides. Prefer deny on ambiguity; never auto-approve.
- **Registry is in-memory.** A runner restart forgets in-flight runs (Hermes
  keeps the truth; sessions are re-listed).

## Rollback

- Steps 1–8: revert the commit; the live stack is untouched.
- Step 9: remove the `"hermes"` entry from the drop-in, `daemon-reload`,
  restart. The runner returns to 12 tools; `hermes-mcp` just stops being spawned.

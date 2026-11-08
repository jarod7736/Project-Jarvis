# TODO — outstanding work from 2026-11-07

Captured at the end of a long session that fixed a live outage and cleaned up
vault storage. Everything below is still open unless marked done. Deep detail
for the storage/centralization items lives in `plans/centralize.md`; the
Lemonade incident is in `plans/lemonade-runbook.md` §2.7.

---

## A. Voice path / firmware — one USB flash covers all of these

Nothing here is urgent; each is a hardening item. They are batched because a
flash is the only way to change them, and the device is USB-only for now (OTA
died at 7% on this weak signal — runbook §4.1).

- [ ] **Confirm the device never reaches Anthropic.** The source is gated
      (`config.h:124 kAnthropicDirectEnabled = false`), and one test showed the
      `claude` intent arriving at the runner (`POST` from `192.168.1.104`), so
      nothing reached `api.anthropic.com`. But the flashed image may predate the
      flag — the proof is "it routed through the runner", which is equally
      consistent with `anth_key` being unset. Verify after flashing, or read
      NVS. `anth_key` can stay provisioned once the flag is on the device.
- [ ] **`kOcHttpTimeoutMs` 12 s is too tight.** Measured: agent path 9.2 s,
      calendar 6.8–16.6 s (the 16.6 s case made two `gcal` calls). Raising it
      costs UX — the FSM blocks behind "Thinking…" — so prefer fixing the two
      server-side causes below before raising this.
- [ ] **Ignore Qwen's `query` for `claude` / `local_llm`.** `IntentRouter`
      dispatches with `query.length() ? query : transcript`, so a 0.5B model's
      paraphrase can replace what the user actually said. Those intents mean
      "hand this text to the smart model" — they should use the transcript.
      This was not today's bug (that was ASR) but it is a second way to corrupt
      a request.
- [ ] **`kOcMaxTokens` 80 → ~512.** Only load-bearing if `OC_PROXY_FORCE_MODEL`
      is ever cleared; today the proxy floors it. Do it in the same flash as
      the §2.6 retirement if that ever happens.

## B. Agent latency margin (server-side, no flash)

- [ ] **Stop the second-guessing tool call.** The calendar run that took 16.6 s
      called `gcal_list_events` twice — the model refines when the first result
      looks thin. A prompt rule ("call it once; if it's empty, say so") removes
      that class.
- [ ] **Wall-clock budget in the agent loop.** `OC_MAX_TURNS=4` bounds turns,
      not time. A ~9 s deadline returning the best answer so far keeps every
      reply inside the device window. Both changes are in
      `tools/oc-personal-runner`, no reflash.

## C. Speech-to-text accuracy

- [ ] **ASR is the remaining weak link.** Proven by observation: "write me a
      haiku" arrived at the model as "write me a high koo", so Jarvis answered
      a nonsense request sensibly and offered to write a poem or story instead.
      Nothing server-side is wrong.
- [ ] **Quantify it.** `SdLogger::logExchange` already records transcript and
      reply for every exchange on the SD card. Pull it and count misrecognitions
      — one anecdote isn't an error rate.
- [ ] **The real fix is a firmware project, not a setting.** Whisper is
      installed on amd-halo and idle. Wiring it needs mic capture on the CoreS3,
      a PSRAM WAV buffer, utterance endpointing, and multipart upload inside a
      blocking single-threaded FSM (`CLAUDE.md`, out-of-scope note). The module
      owns STT today at StackFlow v1.3.

## D. Google plane — one owner, one consent

The goal: kill the recurring re-auth treadmill and stop duplicating Google code
across `oc-personal`, Hermes, `hs-triage` and the cardputer bridge.

- [ ] **Build the owner service on amd-halo.** Holds the single OAuth client and
      token; exposes narrow endpoints (`/calendar/events`, `/gmail/unread`,
      `/gmail/search`) plus `/health` reporting token expiry and last successful
      refresh. `google-mcp` becomes a thin stdio shim over it (the 12-tool agent
      surface is preserved); the other apps call the HTTP API directly.
- [ ] **Never share a token file between two writers** — both rewrite on
      refresh. One owner, many readers, short-lived access tokens.
- [ ] **Alert on refresh failure.** The previous token died 2026-06-06 and the
      cron logged **33 identical `invalid_grant` failures** over five months
      with nobody watching. A push on failure is the whole fix.
- [ ] **Re-auth done 2026-11-07** (token valid, refresh token present, 3
      scopes). The revocation cause is still unknown — plausible: client-secret
      rotation, a manual revoke, or Google's 100-refresh-token cap per client
      (each re-auth mints a new one and silently invalidates the oldest). Not
      the Testing-mode 7-day rule; the app is deployed.

## E. 2ndBrain / storage

Canonical is GitHub `jarod7736/2ndBrain`. Both clones are at `6f97007`, clean.
`02a61df` committed amd-halo's working tree; the merge took origin's post-lint
pages, unioned `index.md`/`log.md`, and recomputed the counts.

- [ ] **Repoint the Windows driver.** `.claude/skills/run-2nd-brain/SKILL.md`
      still names `//synology/backups/Jarod/Jarod Google Drive/vaults/2ndBrain`,
      which no longer exists. Blocked on a decision: where the Windows clone
      lives. **Never git-clone over CIFS** — give it local disk.
- [ ] **Resume the Cloud Sync task.** It is **bidirectional**; both sides are now
      absent, so a resume has nothing to propagate. Do **not** restore the folder
      from the Drive trash — a resumed two-way task pulls it straight back down.
      Confirm the rest of Drive still flows.
- [ ] **Delete `backups/_retired`** (113 MB) and empty the Drive trash after a
      week, once nothing has missed it. It sits outside the task's destination
      root deliberately — inside it, a bidirectional resume would have uploaded
      the whole retired vault back into Drive.
- [ ] **Nightly `git bundle` → Garage** (`2ndbrain-backups/` prefix). Garage is
      for snapshots only; it must never be mounted as a working tree.
- [ ] **Decide the agent's write path for `brain_capture`.** Either the
      amd-halo clone is a `raw/`-only writer with a push credential, or captures
      land in `raw/inbox/` and an ingest step on the writer commits them. The
      second matches the existing `raw/` + `brain_ingest_status` pipeline and
      keeps push credentials off the agent host.

## F. Notifier gap

- [ ] **High-tier pushes are silently dropped when the device is offline.**
      `publish_speak` uses `retain=False` and reports `spoken: true` when paho
      merely accepted the publish; if the device is rebooting there is no
      subscriber and the message vanishes. Observed live on 2026-11-07: first
      push reported success, device stayed silent; a retry after it returned to
      `IDLE` worked. Fix by mirroring the medium tier — check
      `device_state == OFFLINE` and queue instead of publishing. `retain=true`
      is **not** the fix; it replays stale speech on every reconnect.

## G. Move `oc-personal` to amd-halo

Blocked on nothing now that the vault is reconciled. Full inventory and order
of operations in `plans/centralize.md` §5–7.

- [ ] Install `brain-mcp` first, then `google-mcp`, then `oc-personal`; rewrite
      `OC_MCP_SERVERS` paths and `BRAIN_VAULT_PATH`; move the Google creds and
      recreate the token-refresh cron; update NVS `oc_host` to
      `192.168.1.118:8080` (no reflash); verify 12 tools on `/healthz` and one
      real query from the device.
- [ ] Remember the constraint: MCP is stdio-only, so the runner and both MCP
      servers move together.

## H. Housekeeping

- [ ] **Push `main`.** It is 3 commits ahead of GitHub: `efcff3f` (deploy
      default), `9fcf92a` (pending tree: Anthropic severance, docs, comment
      sweep, centralize plan), `59c8e3d` (disable reasoning on passthrough).
      lobsterboy already runs `59c8e3d`, deployed by bundle, clean — a push
      followed by `git pull` there will match exactly.
- [ ] **Captive-portal labels still read "OpenClaw URL" / "OpenClaw Key"**
      (`ConfigSchema.cpp`). User-visible strings, not comments; "Agent Runner
      URL/Key" matches reality.

## I. Brain-ingest CI is broken — parked 2026-11-07

`daily-brain-ingest.yml` has been failing on every run (~20 s each; last six
runs all failed). It runs `claude -p …` with `ANTHROPIC_API_KEY`, which is the
only secret in the repo (created 2026-05-11) — no tokens available, so it has
been dead for a long time.

**This is not a one-line model swap.** The workflow runs on
`ubuntu-latest` (GitHub-hosted), and Lemonade lives on amd-halo at
`192.168.1.118:13305` — LAN and tailnet only. A hosted runner cannot reach it.
**The workflow is disabled at the repo level** (`gh workflow disable
daily-brain-ingest.yml`, 2026-11-07), so the `raw/**` push trigger no longer
queues runs; re-enable with `gh workflow enable daily-brain-ingest.yml` only
once one of the shapes below is in place. The YAML carries a header comment
saying the same, since a repo-level disable is invisible in the file.

Two viable shapes:

- **A. Self-hosted runner on amd-halo (recommended).** `runs-on: [self-hosted,
  …]`, call `http://127.0.0.1:13305` with model `coder`. No tunnel, no exposure,
  no availability cost (amd-halo must be up anyway to serve the model).
  No runner is installed on either box today. `opencode` 1.18.32 is already on
  amd-halo (`~/.opencode/bin/opencode`, config under `~/.config/opencode/`) and
  is the closest drop-in for `claude -p`; the alternative is Claude Code via
  CCR, which they already use elsewhere.
- **B. Hosted runner + Tailscale.** Add `tailscale/github-action` with an
  ephemeral auth key, then reach the tailnet IP. Needs a Tailscale OAuth client,
  a tag, an ACL entry for amd-halo:13305, and new repo secrets. More moving
  parts for no availability gain.

Still open: which harness drives the ingest (opencode vs Claude-via-CCR vs a
bespoke tool-loop script), and whether the ingest should stay LLM-driven at all.

Blocked knock-on: the Mac clone has 6 raw files committed locally (`6f42026`)
and unpushed — pushing them triggers an ingest run that fails until this is
fixed, so hold the push.

---

## Done in this session (context for the above)

- `coder` → `Coder` alias restored in the `lemonade-data` podman volume; both
  passthrough and agent paths verified. No firmware change was needed.
- Google OAuth re-authenticated; calendar returns real data.
- Reasoning disabled on pass-through requests: "write me a haiku" went from
  **22 s with empty content** (spoken as "I can't reach my brain") to **1.3 s
  with real text**. `OC_PROXY_DISABLE_THINKING=1` in the lobsterboy drop-in.
- 2ndBrain reconciled and pushed; Google-Drive-synced copy retired and deleted.
- Notifier high-tier push verified on hardware (device spoke the payload).

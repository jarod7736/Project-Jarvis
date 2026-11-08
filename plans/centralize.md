# Centralize the personal agent on amd-halo

**Status:** captured, not started. Revisit later.
**Captured:** 2026-11-07
**Trigger:** "oc-personal should maybe move to amd-halo to centralize these services."

Everything below was measured on the live boxes on 2026-11-07, not estimated.

---

## 1. Verdict so far

Two claims had to be separated:

- **Resources are not an argument either way.** `oc-personal` is ~140 MB and
  ~0.35% of one core. It could move anywhere.
- **The blocker is not compute.** It is (a) the 2ndBrain vault having diverged
  into two incompatible copies, and (b) MCP being stdio-only, which forces the
  runner and its tool servers onto the same host.

Do not start with the migration. Start with the vault.

---

## 2. Measured footprint of `oc-personal` (lobsterboy)

From `systemctl show oc-personal` and a process census, over a 25-minute run:

| Measure | Value |
|---|---|
| cgroup `MemoryCurrent` | 140,664,832 B (~134 MiB) |
| cgroup `MemoryPeak` | 141,189,120 B (~135 MiB) |
| `systemd-cgtop` memory | 134.1 M |
| `CPUUsageNSec` | 5.19 s over 25 min uptime (~0.35% of one core) |
| Idle CPU, per process | 0.2% / 0.0% / 0.0% |

Per-process RSS:

| Process | RSS | Role |
|---|---|---|
| `oc_personal.server` | 63,756 KiB | FastAPI front + agent loop |
| `brain_mcp.server` | 53,896 KiB | stdio child |
| `google_mcp.server` | 72,992 KiB | stdio child |

Long-run figure from the unit's journal: **50 min 30 s CPU time, 134.6 M peak** —
bursty during agent turns, idle otherwise. Most of the startup CPU is the ~3 s
MCP handshake.

The LLM does the expensive work; the runner only orchestrates tool calls and
forwards prompts.

## 3. amd-halo headroom

| Item | Value |
|---|---|
| CPU | 32 cores, AMD Ryzen AI MAX+ 395 |
| RAM | 125 GiB total, 78 GiB available |
| Load average | 0.22 / 0.26 / 0.27 |
| Disk | 89 GiB free — **96% used, worth watching** |
| Port 8080 | free |
| `loginctl` `Linger` | yes (user units survive logout and boot) |
| python3 | 3.13.5, `venv` OK (repo requires >=3.11) |
| GitHub SSH | authenticated for `jarod7736` |

Also on amd-halo: Lemonade in podman (`lemonade-server`, quadlet-managed),
the model registry, and `~/projects/2ndBrain`.

---

## 4. Structural constraint: MCP is stdio-only

The runner spawns `brain_mcp.server` and `google_mcp.server` as **child
processes** and speaks JSON-RPC over pipes (`OC_MCP_SERVERS` in the drop-in,
`mcp.stdio_client`). There is no socket transport in play.

Therefore "move `oc-personal`" is really "move `oc-personal` + both MCP servers
to one host". They cannot be split.

---

## 5. Dependency inventory

What actually has to move if the runner goes to amd-halo:

| Item | Current location | Note |
|---|---|---|
| Repo checkout | `/home/jarod7736/Project-Jarvis` (lobsterboy) | amd-halo has **no** Project-Jarvis checkout — only `~/projects` |
| `brain-mcp` venv | `tools/brain-mcp/.venv` (lobsterboy) | must be installed **before** `oc-personal` |
| `google-mcp` venv | `tools/google-mcp/.venv` (lobsterboy) | stdio child of the runner |
| Vault | `/srv/2ndbrain` (lobsterboy) | amd-halo has `~/projects/2ndBrain` — **a diverged fork**, see §6 |
| Vault sync | `brain-sync.service` + `.timer`, `git pull --ff-only`, every ~5 min | soft dep: unit has `After=brain-sync.service` |
| Google creds | `~/.config/oc-personal/google-credentials.json`, `google-token.json` | mode 0600 |
| Token refresh | crontab: `0 6 */5 * * …/refresh-token.py …` | move or the token expires |
| `OC_MCP_SERVERS` | drop-in, absolute paths into `/home/jarod7736/Project-Jarvis/...` | must be rewritten |
| Unit + drop-in | `/etc/systemd/system/oc-personal.service` + `.service.d/override.conf` | template has `__RUN_USER__` / `__PROJECT_ROOT__` / `__USER_HOME__`; `deploy.sh install` is interactive |
| Device endpoint | NVS `oc_host` = `192.168.1.178:8080` | change via captive portal / serial provisioning — **no reflash** |
| `notifier` (:8081), `morning-brief` | lobsterboy | stay put |

Notes:

- The tiers probe follows NVS `oc_host`, so moving the runner moves the tier
  check with it automatically. Don't hardcode `amd-halo` in the probe.
- `oc-personal` reads `OC_BACKEND_TOKEN` from its env file; on amd-halo the
  Lemonade key is already present in container config, but the runner needs its
  own copy in an `EnvironmentFile` (mode 0600, not in git).
- `deploy.sh install` prompts with `read -p` — do not run it over a
  non-interactive SSH session.

---

## 6. THE BLOCKER: two diverged 2ndBrain copies

Canonical source is `git@github.com:jarod7736/2ndBrain.git`.

| | amd-halo `~/projects/2ndBrain` | lobsterboy `/srv/2ndbrain` |
|---|---|---|
| HEAD | `13b37d7` — 2026-10-26 "ingest: 7 sources — Lemonade alias tooling + homelab notes" | `7d8565f` — 2026-10-25 "update: OpenChamber boot services on amd-halo" |
| vs origin | ahead 2, **behind 10** | exactly at origin tip |
| `origin/main` ref | stale (`f8fe501`, Sep 20) | fresh (fetched every ~5 min) |
| Uncommitted | **13 entries + 8 untracked** daily briefs | 2 notes from Jul 25 |
| Locally edited | `log.md` 2026-11-05, `index.md` 2026-11-05 | no local writes |
| Size | 63 M | 90 M |
| Remote | `git@github.com:…` | `git@github-2ndbrain:…` |

**Fork point: `f8fe501` (2026-09-20).** amd-halo has not integrated origin since.

- amd-halo's 2 unique commits: "skills: dedupe and normalize vault skill copies"
  (Sep 26) and "ingest: 7 sources" (Oct 26).
- amd-halo is missing 10 origin commits: Oct 25 OpenChamber, Oct 1 + Sep 30
  nightlies, Sep 28 "high school app planning", the Sep 26 lint series, an
  "ingest: 6 homelab notes" commit, and a merge PR.
- amd-halo's dirty tree includes `D "Deliberate Practice.md"`, `M index.md`,
  `M log.md`, `M skills/brain-design/SKILL.md`, `M skills/brain-update/SKILL.md`,
  and 8 untracked `raw/notes/daily_brief_*.md`.
- The two raw notes differ across hosts:
  `raw/notes/lemonade-model-aliases.md` is sha `5821877b…` on lobsterboy and
  `72bfe0e8…` on amd-halo. Whether lobsterboy's July edits were folded into the
  Oct 26 ingest is **not** answerable from hashes alone — read the diff.

**Consequence:** pointing `brain-mcp` at amd-halo's tree today would make the
agent answer from a vault missing three weeks of committed notes. It would get
worse, not fresher. And `pull --ff-only` on amd-halo would **fail** — it is a
merge, not a fast-forward.

---

## 7. Order of operations

Reconcile first, migrate second. Do not invert.

- [ ] **7.1 Back up both trees** (`tar czf` outside the repos) and record the
      exact HEADs.
- [ ] **7.2 Triage amd-halo's dirty tree.** Decide on the
      `Deliberate Practice.md` deletion and the 8 untracked daily briefs; commit
      the rest **on a branch**, not on `main`.
- [ ] **7.3 Fetch + merge** `origin/main` into amd-halo's branch. Expect
      conflicts in `index.md` / `log.md`; resolve deliberately.
- [ ] **7.4 Reconcile the two raw notes.** Diff
      `raw/notes/lemonade-model-aliases.md` and
      `raw/notes/lemonade-aliases-how-it-works.md` across hosts and pick a
      winner. Small diff (~36 insertions / 22 deletions).
- [ ] **7.5 Push**, and confirm origin now contains amd-halo's work.
- [ ] **7.6 Choose ONE canonical writer.** Two live writers against a
      `pull --ff-only` mirror guarantees conflicts. Either amd-halo becomes
      canonical (move `brain-sync` there, stop lobsterboy's timer, make
      `/srv/2ndbrain` read-only or delete it) **or** lobsterboy stays canonical
      (then reconcile amd-halo back to it and stop editing amd-halo's copy).
- [ ] **7.7 Only now: migrate `oc-personal`.**
      1. Clone the repo to amd-halo (`~/projects/Project-Jarvis` or similar).
      2. Install `brain-mcp` first (its venv is a prerequisite of `oc-personal`).
      3. Install `google-mcp`; copy creds to `~/.config/oc-personal/` (0600).
      4. Install `oc-personal`; rewrite `OC_MCP_SERVERS` paths and
         `BRAIN_VAULT_PATH=~/projects/2ndBrain`.
      5. Recreate the Google token-refresh cron.
      6. Start it; expect 12 tools and `agent_ready: true` on `/healthz`.
- [ ] **7.8 Repoint the device.** NVS `oc_host` → `http://192.168.1.118:8080`
      via captive portal or serial provisioning. No reflash.
- [ ] **7.9 Verify on hardware:** wake word, one passthrough question, one
      `personal_query`, one calendar or inbox read (proves OAuth survived).
- [ ] **7.10 Retire the lobsterboy copy** of `oc-personal` (unit + drop-in).
      Keep the drop-in backup until the new box has run for a week.

---

## 8. Rollback

| To undo | Do this |
|---|---|
| Device pointing at amd-halo | Set NVS `oc_host` back to `http://192.168.1.178:8080` |
| Runner moved | Re-enable + start `oc-personal` on lobsterboy (drop-in backup in `/tmp`) |
| Vault merge | `git reset --hard <recorded HEAD>` on the branch, restore the tarball |

---

## 9. Open questions (need a human decision)

1. Are amd-halo's unbacked `log.md` / `index.md` edits from Nov 5 wanted, or are
   they scratch work?
2. Should `Deliberate Practice.md` really be deleted, or is that an accidental
   `rm`?
3. Which version of the two `raw/notes/lemonade-*.md` files wins?
4. ~~Which box owns the vault long-term~~ — **decided 2026-11-07: GitHub
   `jarod7736/2ndBrain` is canonical.** No host owns it; every host holds a
   clone. See §12.
5. Does anything else silently depend on the vault path `/srv/2ndbrain`
   (schedulers, other tools, cron)?

---

## 10. Why this is worth doing at all

The measured resources are a rounding error, so the case is architectural, not
resource-driven:

- One box owning "model + agent" removes a network hop and a second host from
  the reasoning path.
- Lobsterboy would go back to being the notification / MQTT / HA bridge, which
  is a coherent role.
- The vault is already on amd-halo, and it is a small repo (63–90 MB) that
  changes rarely — so the data is not really tethered to lobsterboy. My first
  read of this was wrong: it is a git clone, not local-only state.

The cost is real but bounded: one merge of a diverged vault, one venv ordering
constraint, one OAuth token and its cron, and an NVS change on the device.

---

## 11. References

- `plans/lemonade-runbook.md` §2.7 — the `coder` alias outage and where model
  aliases actually live (podman named volume `lemonade-data`).
- `CLAUDE.md` — tier probe follows `oc_host`; stdio MCP; `deploy.sh` template
  substitution; `brain-mcp` must be installed before `oc-personal`.
- `AGENTS.md` — no test/lint/CI anywhere; `deploy.sh test` is the smoke path.
- Lemonade: `192.168.1.118:13305`, alias `coder` → `Coder`, container
  `lemonade-server`, volume `lemonade-data`.
- Runner: `192.168.1.178:8080` (today), `/healthz` reports `agent_model`,
  `proxy_model`, 12 tools.

---

## 12. Storage model — decided 2026-11-07

**Canonical source of truth: GitHub `jarod7736/2ndBrain` (origin).** No host owns
the vault; every host holds a clone. That is a stronger rule than "keep the
copies in sync", because the failure mode was never missing sync — it was
*writes that bypassed git entirely*.

The rule that follows:

- **Nothing writes the vault through a filesystem sync.** No Cloud Sync, no
  Google Drive folder, no shared-folder editing. Git is the only transport.
- **No git repository goes inside a cloud-synced folder — ever** (owner,
  2026-11-07). Not the vault, not any other repo. A sync daemon rewrites `.git`
  internals underneath a running git process, and two sync mechanisms over one
  tree cannot be reconciled: one of them always wins silently. The failure we
  just cleaned up was exactly this, so treat it as a standing rule rather than
  a lesson from one incident. Repo backups go to Garage (`git bundle`), which
  is object storage and never mounted as a working tree.
- Every copy is a clone with exactly one role:
  - **publish writer** — amd-halo. The only place `wiki/`, `index.md` and
    `log.md` are written; runs ingest and pushes the result.
  - **raw contributor** — Mac and Windows. Pulls the wiki, and may commit
    **only `raw/`**: clip, drop a file, push. Ingest on amd-halo does the rest.
  - **read-only mirror** — anything else (lobsterboy today, until retired).
- **Exactly one writer, and one git actor per checkout.** amd-halo's clone is the
  designated writer — it runs the ingest service *and* hosts the agent, so
  `brain-mcp`'s `brain_capture` lands in the same `raw/` the ingest publishes
  from. The agent writes files; the publish service owns every `git` command.
  Two git actors on one tree is precisely how the old divergence happened.
- If two writers collide, git reports it at push time. That is the desired
  behaviour — it is what the current setup lacks.
- Garage is for **snapshots, not the working tree**: nightly `git bundle` (or
  tar) into a `2ndbrain-backups/` prefix. Object storage cannot merge, lock, or
  represent an Obsidian working copy; mounting it as a filesystem with multiple
  writers is how you corrupt Markdown.

Copy inventory as of 2026-11-07, and the fate of each:

| Copy | Today | Target |
|---|---|---|
| GitHub `jarod7736/2ndBrain` | origin, 1 commit behind amd-halo's unpublished work | **canonical** |
| amd-halo `~/projects/2ndBrain` | fork at `f8fe501`, ahead 2 / behind 10, 13 dirty, edited Nov 5 | clone that tracks origin; the agent's read path |
| lobsterboy `/srv/2ndbrain` | mirror of origin, `brain-sync` every 5 min | read-only puller; may be unnecessary once the agent moves to amd-halo |
| `//synology/backups/Jarod/Jarod Google Drive/vaults/2ndBrain` | **Google-Drive-synced**, used by the Windows Claude driver | **Cloud Sync task removed**; see below |
| `\\synology.holdfast.lan\homes\jarod7736\2nd Brain` | skills' default path | repoint to the new clone |

### 12.1 Removing the Google Drive copy

It is not a stray folder — `wiki/entities/synology-ds920.md` documents it as the
vault's home, and `.claude/skills/run-2nd-brain/SKILL.md` writes there over
UNC. So the removal is a **re-home**, in this order:

- [ ] Reconcile the two git copies first (§7.2–7.6). "Canonical" is not real
      until amd-halo's 2 unpublished commits and 13 dirty files are dealt with.
- [ ] **Do not git-clone over CIFS.** Give the Windows driver a local clone
      (e.g. `C:\Users\jarod\2ndBrain`) and change the skill's two UNC paths to
      that local path. Git over SMB is slow and its locking is unreliable.
- [x] **Cloud Sync paused by owner 2026-11-07**, then the folder was deleted in
      Google Drive itself. The task is **bidirectional**, which makes the order
      matter: a two-way task treats "present remotely, absent locally" as a
      deletion and would have pushed it upstream, so deleting it in Drive *while
      paused* was the right move. Both sides are now absent, so the pair is
      consistent and the task can be resumed without propagating anything.
      Caveat: do **not** restore it from the Drive trash — that reintroduces it
      remotely and a resumed two-way task will pull it straight back down.
- [ ] Repoint every reference: `.claude/skills/run-2nd-brain/SKILL.md` (two
      UNC paths), the skills' default `\\synology.holdfast.lan\homes\...`,
      and the `synology-ds920.md` role description.
- [x] **Inventoried before touching it (2026-11-07).** It was a real git clone
      of the same GitHub remote at `74499a6` (2026-10-01), *ahead 0* and behind
      1. Its only dirty files were editor state — `.obsidian/workspace.json`,
      `.obsidian/plugins/large-language-models/data.json`, and three
      `.smart-env/**.ajson` — nothing under `raw/`, `wiki/`, `log.md` or
      `index.md`. So it held no unique vault content.
- [x] **Retired, not deleted.** Renamed to
      `backups/_retired/2ndBrain-gdrive-20261107-150247` (113 MB, intact) with
      the dirty diff recorded alongside at
      `_retired/gdrive-2ndbrain-record-20261107/`. Note it was initially parked
      at `backups/Jarod/_retired` and had to be moved one level up: the task's
      local destination root is under `backups/Jarod`, so on a resumed
      **bidirectional** task those 113 MB would have been uploaded into Drive as
      a new folder. Delete it after a week if nothing has missed it.
- Backups of the two live clones before reconciliation:
      `~/2ndbrain-backups/` on both hosts — `git bundle --all` plus a worktree
      tarball (27 MB + 9 MB each).

### 12.2 The agent's write path

`brain_capture` is the one place the agent must write. Two workable options,
decided later:

1. The amd-halo clone is a **writer** to `raw/` only; it commits and pushes
   after a capture. Needs a push credential on that box.
2. The agent writes to a `raw/inbox/` area that an ingest step on the writer
   commits and pushes. No push credential on the agent host, but captures are
   not durable until ingest runs.

Option 2 matches the existing `raw/` + `brain_ingest_status` pipeline and keeps
the agent out of write-access territory; option 1 is simpler if captures must
land immediately.

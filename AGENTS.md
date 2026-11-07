# AGENTS.md

`CLAUDE.md` in this repo is the authoritative deep reference: architectural
invariants, NVS schema, endpoint contracts, error taxonomy. Read it before
non-trivial work. This file adds only what `CLAUDE.md` omits or gets wrong for
a fresh agent, plus the executable facts you would otherwise guess at.

## Read this before you act

- **`agentos` is not installed on this machine.** `CLAUDE.md` opens by
  instructing you to run `agentos --json --project project-jarvis project show`
  and to record every architectural decision with `agentos ... decision add`.
  `command -v agentos` returns nothing. Do not stall on it and do not assume the
  status board is reachable — fall back to `CLAUDE.md`'s "Where to read first"
  list.
- **`pio` is on `PATH` on this Mac** (`/opt/homebrew/bin/pio`, Homebrew), so
  plain `pio run` from the repo root works. `~/.pio-venv/bin/pio` does not
  exist here. `scripts/build.sh` still needs `PIO` overridden — see the build
  section.

## No test, lint, or CI infrastructure

Verified absent repo-wide: `.github/`, `Makefile`, `pytest.ini`, `tox.ini`,
`conftest.py`, any `test_*.py`, `.pre-commit-config.yaml`, and any
`[tool.pytest]` / `[tool.ruff]` / `[tool.mypy]` / `[tool.black]` section. Do not
spend time looking for a suite to run.

The real verification paths are:
- **Firmware:** nothing proves a change until it runs on the CoreS3. A clean
  compile is not evidence. Compile is the only local signal.
- **Python tools:** `tools/<name>/deploy.sh test` (smoke tests hitting the live
  service). See below.

## Firmware build

PlatformIO, ESP32-S3, M5Stack CoreS3 + M5Module-LLM. `platformio.ini` sets
`default_envs = cores3`, so `-e` is optional.

```bash
pio run                    # build
pio run -t upload          # flash firmware
pio run -t uploadfs        # flash LittleFS image — required for data/web changes
pio run -t monitor         # serial monitor
```

`scripts/build.sh [run|upload|uploadfs|monitor|clean]` wraps these. Its default
`PIO` is a **Windows Git-Bash `platformio.exe`**, and it copies sources into a
Windows temp dir as a workaround for Windows pio hanging on `\\wsl.localhost\`
SMB paths. On macOS or Linux that path does not exist and the script exits with
`ERROR: PlatformIO not found` even when `pio` is on `PATH` — the script never
consults `PATH`. Always override:

```bash
PIO=$(command -v pio) ./scripts/build.sh run
```

`upload_port` / `monitor_port` are pinned to `COM9` (Windows). Override for
macOS/Linux. OTA upload needs the commented `espota` block uncommented **and**
`tailscale set --accept-routes=false` first, or Tailscale poaches espota traffic
(`plans/phase7-validation.md` Gate 2).

`data/web/` (captive-portal UI) is packed into LittleFS — a normal `upload`
does not ship it. Use `uploadfs`.

## Python tools

Five Python tool packages under `tools/`, deployed to lobsterboy:
`brain-mcp`, `google-mcp`, `morning-brief`, `notifier`, `oc-personal-runner`.
Only four ship a systemd unit — see below.

Shared convention: `src/` layout, `hatchling`, `requires-python = ">=3.11"`, a
`[project.scripts]` console entry, and an editable install into a per-tool venv
at `tools/<name>/.venv` (gitignored). There is no shared root project and no
workspace file — each tool is standalone.

```bash
tools/<name>/deploy.sh survey    # read-only audit; the default subcommand
tools/<name>/deploy.sh install   # venv + pip install -e, writes systemd units (interactive, sudo)
tools/<name>/deploy.sh test      # smoke test against the running service
```

Things that will bite:
- Units in `tools/<name>/systemd/` are **templates**, not installable files.
  `deploy.sh` substitutes `__RUN_USER__` (plus `__PROJECT_ROOT__` /
  `__USER_HOME__` where the unit uses them) before writing to
  `/etc/systemd/system`. Only `brain-mcp` and `oc-personal-runner` then verify
  that no `__PLACEHOLDER__` survived before installing; `morning-brief` and
  `notifier` do not. Copying a template in by hand without substitution
  crash-loops the service.
- `oc-personal-runner` requires `brain-mcp`'s venv to already exist. Install
  `brain-mcp` first.
- `oc-personal-runner` reads `OC_BACKEND_TOKEN` from
  `/etc/oc-personal/secrets.env` (0600), not from the unit file.
- `google-mcp` ships **no systemd unit** — it is a stdio child of
  `oc-personal-runner` (referenced via `OC_MCP_SERVERS`). Its `deploy.sh` only
  builds the venv, and it needs a one-time OAuth consent
  (`.venv/bin/google-mcp-auth`) plus client secrets at
  `~/.config/oc-personal/google-credentials.json`; the token lands in
  `~/.config/oc-personal/google-token.json`.
- Unit shapes: `brain-mcp` and `morning-brief` install a `.service` + `.timer`;
  `notifier` and `oc-personal-runner` install a `.service` only.

## Repo guards

- **This is not an npm project.** `.gitignore` deliberately ignores
  `node_modules/`, `package.json`, and `package-lock.json` at the root to stop
  tooling from seeding Node config. Do not add Node tooling.
- `docs/` and `.claude/` are gitignored and local-only (reference PDFs, per-user
  Claude settings). Never assume another checkout has them.
- `.vscode/extensions.json` recommends the PlatformIO IDE extension and lists
  the C++ extension pack as explicitly unwanted.

## Documents that are not current

- `PLAN.md` (76 KB) is a historical design doc. Per-phase prose is superseded by
  the retro at the end of each section. `CLAUDE.md` already flags this; it is
  easy to miss and expensive to get wrong.
- `README.md` lags recent Sprints. `git log --oneline -20` is the better signal
  for what actually shipped.

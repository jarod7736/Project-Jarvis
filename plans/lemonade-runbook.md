# Lemonade cutover — runbook

**Date:** 2026-07-22
**Related:** PR #63, design spec `plans/lemonade-llm-tts.md`

Operational state and remaining tasks for moving Jarvis onto Lemonade
(`amd-halo`) for LLM and TTS.

---

## 1. Current state — DONE and verified

| Item | State | Evidence |
|---|---|---|
| `oc-personal` passthrough → Lemonade | **live** | `/healthz` reports `backend: http://amd-halo:13305` |
| gpt-oss load-on-demand (was HTTP 500) | **live** | unloaded the model to force the failure; next request self-healed in 18.5 s |
| Warm LLM path | **live** | 0.87 s end-to-end |
| MCP agent loop (`oc-personal`) | **live** | 3.75 s, returned 5 real projects |
| CoreS3 firmware (kokoro TTS + tier fallback) | **flashed** | USB flash, hash verified |
| `lemo_*` NVS provisioning | **done** | `[PROV] Saved lemo_key (64 chars…)` |
| kokoro TTS on device | **live** | `speak: lemonade TTS in flight`, 86 KB MP3 played |
| Passthrough model rewrite | **live** | `OC_PROXY_FORCE_MODEL=coder` (was `chat` — see §2.4) |

Device: `jarvis.local` / **192.168.1.104**. Lemonade: **192.168.1.118:13305**.

---

## 2. lobsterboy — WHAT NEEDS DOING

### 2.1 Return to `main` after PR #63 merges  ← DONE 2026-08-30

lobsterboy is back on `main`; `oc-personal.service` restarted and healthy
(`/healthz` reports `backend: http://amd-halo:13305`).

### 2.2 Facts worth knowing for any future deploy here

- **Python deploys are just git + restart.** The tools are pip-installed
  **editable**, so the venv resolves straight to the checkout. No
  `pip install`, and no need to re-run `deploy.sh` unless the systemd unit
  or venv dependencies changed.
- **Do not run `deploy.sh install` over a non-interactive SSH session** —
  it prompts with `read -p` and will hang.
- **Sudo is passwordless for `/bin/systemctl` only.** Anything else needs
  an interactive password.
- **Config lives in the drop-in**, not the base unit:
  `/etc/systemd/system/oc-personal.service.d/override.conf`. It sets
  `OC_BACKEND_URL` and `OC_PROXY_FORCE_MODEL`. The deployed `OC_BACKEND_URL`
  is `http://192.168.1.118:13305` — the LAN IP, *not* the `amd-halo`
  MagicDNS name the repo default and the drop-in's own comment suggest
  (verified against `/healthz`, 2026-09-20).
  `OC_PROXY_FORCE_MODEL` was `gpt-oss-120b-Q4_K_M` when this runbook was
  written and is now `coder` (§2.4). **The drop-in overrides the repo
  default, so editing `config.py` alone changes nothing on lobsterboy.**
  `/healthz` is the only authority on the live value.
- **The Lemonade API key on lobsterboy is `OC_BACKEND_TOKEN`** in
  `/home/jarod7736/.config/oc-personal/secrets.env` (same 64-char value as
  1Password `holdfast-lan` → "lemonade api key").

### 2.3 Nothing else to do on lobsterboy

The load-on-demand fix is deployed and verified. No unit changes, no new
secrets, no new services. Still true as of §2.5 — that section's changes
are repo-only and need no deploy.

### 2.4 2026-08-30 — passthrough 404 fixed + OpenClaw decommissioned

- **`OC_PROXY_FORCE_MODEL` was `chat`, which does not exist on Lemonade.**
  Every non-`oc-personal` request (the firmware's `local_llm`/`claude`
  intents) returned `(upstream: backend HTTP 404)`. Changed the override to
  `coder` (a Lemonade tool-calling alias, ~4s) and restarted the service.
  `tools/oc-personal-runner/src/oc_personal/config.py` default updated to
  match. The agent path already used `coder` and was unaffected.
- **OpenClaw decommissioned.** All OpenClaw user units/timers stopped and
  removed, binaries deleted, tailscale-serve `/` route dropped. The runner
  never depended on it. `~/.openclaw/` data kept (inert). The daily AMD-brief
  cron (which lived under `~/.openclaw/tools/`) was removed.
- **Morning brief stopped** — agent response (~56s) exceeded the 60s timeout.

### 2.5 2026-09-20 — Anthropic severed; `coder` confirmed for both paths

**No deploy required.** Both changes are repo-only; the running service
already matches.

- **Firmware: direct-to-Anthropic severed.**
  `config::kAnthropicDirectEnabled = false` (`src/config.h`) gates the
  single dispatch site in `src/app/IntentRouter.cpp`. `AnthropicClient`,
  NVS `anth_key`, the `NVSConfig` accessors, and the captive-portal field
  are all retained — the flag is the only thing between them and a live
  call, and flipping it back restores the path. **Not compile-verified:**
  `pio` is not installed on the Mac this was written from.
- **`OC_PROXY_FORCE_MODEL` stays `coder`.** Briefly changed to
  `gpt-oss-120b-Q4_K_M` in this session on the reasoning that the
  passthrough serves plain chat and doesn't need a tool-calling model.
  Reverted by decision: one alias for both paths keeps the stack on a
  single resident model and leaves an LLM slot free. The passthrough
  genuinely does not tool-call — only the agent loop does — so the
  argument for splitting was sound, but simplicity won.

**Verified against `/api/v1/health` on 2026-09-20:**

| Alias | Checkpoint | State |
|---|---|---|
| `coder` | `unsloth/Qwen3.8-27B-GGUF:UD-Q4_K_M` | loaded, ready, ctx 65536 |
| `gpt-oss-120b-Q4_K_M` | `/var/cache/models/lemonade/gpt-oss-120b-Q4_K_M` | loaded, ready, ctx 95751 |
| `kokoro-v1` | `mikkoph/kokoro-onnx` | loaded, ready (tts slot) |

Three facts worth carrying forward:

1. **`coder` is not a Coder model.** The alias now resolves to
   Qwen3.8-27B — upgraded from the Qwen3-Coder-30B the design doc
   recorded, with no change needed on lobsterboy or the device. That
   re-pointability is the reason everything references the alias, but it
   means the name tells you nothing about which weights answer. Always
   check `/api/v1/health` rather than trusting a checkpoint in a doc.
2. **Nothing is `pinned`.** Both LLM slots are full (`max_models.llm` is
   2) and eviction is by LRU, so a third LLM name arriving at Lemonade
   drops one. With everything on `coder`, gpt-oss is the expendable one.
3. **Lemonade is now 11.5.0**; `plans/lemonade-llm-tts.md` records 10.5.1
   from design time, and the deployed `OC_BACKEND_URL` is the LAN IP, not
   the `amd-halo` MagicDNS name that file and the drop-in comment imply.

**Firmware consequence worth knowing:** severing Anthropic leaves
HOTSPOT_ONLY with no backend for the `claude` intent, so it now speaks
the canned "can't reach my brain" line on that tier. Previously it was
the one tier where `claude` still worked. Restore by flipping the flag.

### 2.6 Why the runner stays in the LLM path (decided 2026-09-20)

Recurring question: if TTS already goes straight to amd-halo and the
intent routing is already on-device, why does `local_llm` / `claude` hop
through lobsterboy at all? Answer, so it isn't re-derived:

**The passthrough carries three things, and only some are real.**

| What `proxy.py` does | Still load-bearing? |
|---|---|
| Floors `max_tokens` 80 → 512 | **Yes.** Measured 2026-09-20. |
| Rewrites the model name | Only because the flashed firmware sends `gpt-oss-120b-Q4_K_M` and we want `coder`. |
| Load-on-demand retry (`backend.py`) | Probably moot. It existed for gpt-oss's local-path registration (§3.1); `coder` resolves to a normal HF repo id and should autoload. Unverified — not worth unloading a live model to prove. |

**The measurement.** `src/net/LLMClient.cpp:208` sends
`max_tokens = kOcMaxTokens = 80`, a compile-time constant. Sending that
straight at Lemonade with `model=coder`:

```
finish_reason: length
content: "...scatter shorter wavelengths of sunlight (blue and violet)"   <- cut mid-sentence
```

The floor is the only thing preventing the device from speaking
truncated sentences. Note the `PROXY_MIN_MAX_TOKENS` comment predicts an
*empty* reply; that was measured against `chat`, which no longer exists.
On `coder` the failure mode is truncation, not silence.

**Why not just bypass it anyway.** The agent loop (`oc-personal`, 12 MCP
tools) cannot move to Lemonade — Lemonade serves models, it does not host
tools. So lobsterboy stays in the picture regardless. Bypassing the
passthrough would take the device from one backend address in NVS to two,
with two failure modes, to save a LAN hop worth milliseconds.

**Clean retirement path**, if you want it gone — all of it needs a USB
reflash, so batch it with the next flash:

1. Raise `kOcMaxTokens` (~512) and set `kOcLocalModel = "coder"` in
   `src/config.h`.
2. Set `OC_PROXY_FORCE_MODEL=""` in the drop-in.
3. Leave `proxy.py` in place, inert — same sever-don't-delete move used
   for `AnthropicClient`.

Until step 1 ships to the device, steps 2–3 will truncate every spoken
reply. Do not do them independently.

### 2.7 2026-11-07 — `coder` alias vanished after a Lemonade upgrade

**Symptom:** every voice turn came back as `(upstream: backend HTTP 404)`
(passthrough) or `(error: backend HTTP 404)` (agent). Both LLM paths dead at
once; TTS unaffected. Not a greeting bug — "hello" was just the first thing
anyone said after the upgrade.

**Cause:** Lemonade on amd-halo is now **2026.45.3** (was 11.5.0), and model
ids became **case-sensitive**. `GET /api/v1/models/coder` returns
`model_not_found`; `Coder` returns 200 and resolves to the *same* checkpoint
the alias has served since §2.5 — `unsloth/Qwen3.8-27B-GGUF:UD-Q4_K_M`,
ctx 262144, labels `chat/custom/tool-calling/mtp`. Both
`OC_PROXY_FORCE_MODEL` and `OC_AGENT_MODEL` were `coder`, so every request
404'd. The error code is `model_not_found`, **not** `model_load_error`, so
`backend.py`'s load-on-demand retry never fired — and would not have helped,
since the id is absent rather than unloaded.

**Fix (applied) — no rename anywhere:** created a real alias on amd-halo so
*both* ids exist:

```bash
curl -X POST http://192.168.1.118:13305/internal/aliases \
  -H "Authorization: Bearer $LEMONADE_API_KEY" \
  -H 'Content-Type: application/json' \
  -d '{"alias": "coder", "target": "Coder"}'
```

`GET /api/v1/models/coder` now returns 200 and resolves to the same
checkpoint. Aliases live in `<cache_dir>/aliases.json` and survive a `lemond`
restart; the CLI form is `lemonade alias add coder Coder`. This costs **no
extra model slot** — the alias stands in for its target in the `model` field,
so `all_models_loaded` still shows only the one `Coder` entry (measured).

**Where it actually lives (verified 2026-11-07):** Lemonade runs as a **podman
container**, `lemonade-server` (`ghcr.io/lemonade-sdk/lemonade-server:latest`),
managed by the quadlet `~/.config/containers/systemd/lemonade-server.container`.
The alias therefore sits in the container's view of its cache dir, but that dir
is the podman **named volume `lemonade-data`** mounted at
`/opt/lemonade/.cache/lemonade`, so on the host it is
`~/.local/share/containers/storage/volumes/lemonade-data/_data/aliases.json`
(now `{"coder": "Coder"}`). A named volume survives container recreation and
image upgrades; it does **not** survive `podman volume prune`.

**The Alias Manager does not apply here.** amd-halo also runs
`lemonade-aliases.service` (`~/projects/lemonade-aliases`, tailnet-only on
`100.120.121.35:13310`) — a declarative UI that owns
`~/.config/lemonade/aliases/<name>.json` and applies them with
`lemonade import`. **It does not work against the podman container** (owner,
2026-11-07), so do not route aliases through it; its `/api/aliases` reports
`aliases: []` and ignores `coder`. The supported path on this box is
`POST /internal/aliases` writing `aliases.json` in the `lemonade-data`
volume.

Consequence to keep in mind: the alias is runtime state. It survives container
recreation because the volume does, but nothing declares it — after
`podman volume prune`, or on a fresh box, it must be re-created with the POST
above. That is a one-line recovery, but it is the difference between the runner
working and returning `model_not_found`.

Two constraints worth remembering: an alias may not shadow a registered model
name (409 `alias_conflict`) — `coder` works precisely because the upgrade left
it unregistered — and aliases are resolved by the OpenAI-compatible surface
only, not by the Ollama/Anthropic gateways. The authoritative spec is served
by the box itself at `/v1/docs/api/internal`, which is version-exact and beats
any doc in this repo.

**State after the fix:** the lobsterboy drop-in is back to
`OC_PROXY_FORCE_MODEL=coder` / `OC_AGENT_MODEL=coder`, so live and repo agree
again (§2.2); repo defaults were never changed. Verified end-to-end:
passthrough and agent both answer. A pre-change backup of the drop-in is at
`/tmp/oc-personal.override.conf.bak.*`.

No firmware change on either path: the device's baked-in `kOcLocalModel` is
rewritten by the proxy, so this needs no reflash.

**Lesson:** `/api/v1/models` is the authority on ids, and ids are
case-sensitive as of 2026.45.3. A Lemonade upgrade can rename the alias out
from under both paths at once — this is the second model-name drift (after
`chat`, §2.4) to take the whole LLM path down. A `/healthz`-driven synthetic
turn would catch it before a user does.

---

## 3. amd-halo — WHAT NEEDS DOING

### 3.1 Repair the gpt-oss registration (open)

`gpt-oss-120b-Q4_K_M` is registered by **local path**
(`/var/cache/models/lemonade/…`), the extra-dir pattern the vault runbook
warns against. Consequences:

- implicit chat-triggered load → resolves against Hugging Face → **404**
- `POST /api/v1/pull` → same **404**
- only `POST /api/v1/load` works, and it does **not** survive a `lemond`
  restart

The proxy now works around this for traffic through `oc-personal`.
**Clients that hit Lemonade directly — Claude Code via CCR — still get the
404** until the registration is repaired.

Fix is to register it from the catalog rather than by path. Needs console
or SSH access to amd-halo: SSH from lobsterboy currently fails with
`Host key verification failed`, so this could not be done remotely.

### 3.2 Do not "fix" the context size

Already correct. `ctx_size: 131072` lives in
`/var/lib/lemonade/.cache/lemonade/config.json` — **not** the
`LEMONADE_CTX_SIZE` env var in `/etc/lemonade/lemonade.conf`, which is
dead config.

### 3.3 Capacity note

`max_models` is 2 per type (`llm`, `tts`, `transcription`). `coder` and
gpt-oss now sit resident together; a third LLM request evicts by LRU. If
Claude Code suddenly slows down, check whether something evicted `coder`.

Refer to the **alias**, not the checkpoint — the weights behind `coder`
have already been upgraded once (Qwen3-Coder-30B → Qwen3.8-27B) without
anything on lobsterboy or the device changing, which is the whole point
of pointing at aliases. `/api/v1/health` reports the live checkpoint.

---

## 4. Device (CoreS3) — WHAT NEEDS DOING

### 4.1 Weak WiFi is the main operational risk (open)

Measured RSSI ranged **-68 to -80 dBm**. This already caused a real
failure: **OTA died at 7%**. With 0% packet loss but 29–615 ms RTT
(ESP32 modem-sleep plus weak signal), espota's lockstep ACKs crawled at
~1.1 KB/s.

Consequence: **treat OTA as unreliable on this device until the signal
improves.** USB flashing works fine and takes ~9 s.

Options: move the device closer to an AP, add an AP/repeater near it, or
accept USB-only flashing.

### 4.2 Flashing over USB (the reliable path)

The device is on **COM9** (Espressif VID `303a:1001`). WSL2 cannot use it
directly — `usbipd` attach is blocked by Windows Firewall on TCP 3240 —
so flash from Windows, which already has PlatformIO installed.

Four images are required, not just the app. `boot_app0.bin` is what forces
boot from slot 0; without it a device that last took an OTA can boot the
*old* image from slot 1 and look like the flash silently failed.

```
0x0      bootloader.bin
0x8000   partitions.bin
0xe000   boot_app0.bin      <- forces boot from app0
0x10000  firmware.bin
```

Use `--flash_mode keep --flash_freq keep --flash_size keep` so esptool
leaves the header values the build already baked in.

**NVS is preserved.** It occupies 0x9000–0xdFFF, outside every region
written above, so WiFi credentials, `ota_pass`, and TTS keys survive a
reflash.

### 4.3 Provisioning new NVS keys

As of PR #63 the serial provisioning window also opens when `lemo_key` is
missing, and accepts `lemo_host` / `lemo_key` / `lemo_voice` (plus
`tts_provider` / `tts_proact`, which were already supported).

Send a bag-of-keys JSON over serial during the 30 s window at boot. A
payload carrying **`ssid` but no `pass`** satisfies the caller's
ssid-required check **without overwriting stored WiFi credentials** — so
you don't need to know the WiFi password to provision other keys.

Gotcha: opening the serial port with pyserial asserts DTR and **resets the
board**. Pre-clear `dtr`/`rts` before `open()` to monitor a running device
without rebooting it.

The alternative provisioning route is the captive portal: 2-second
long-press on the touchscreen → join WiFi `Jarvis-Setup` → `http://192.168.4.1/`.

### 4.4 Current device config

| Key | Value |
|---|---|
| `tts_provider` | `lemonade` |
| `tts_proact` | `lemonade` |
| `lemo_host` | `192.168.1.118:13305` |
| `lemo_voice` | `af_sky` |
| `lemo_key` | set (64 chars) |

Cloud TTS credentials (`tts_api_key`) are untouched and still work — they
are the off-LAN fallback.

---

## 5. Repo / docs — WHAT NEEDS DOING

- **Merge PR #63**, then do §2.1.
- **2ndBrain correction (open):** `wiki/analyses/lemonade-model-runbook.md`
  gives amd-halo as `192.168.1.61`, which does not answer. The live address
  is **192.168.1.118**. Left unedited because that vault is a separate repo.
- **STT is not built.** `Whisper-Large-v3-Turbo` is installed on amd-halo
  and ready, but routing STT to it needs CoreS3 mic capture, PSRAM WAV
  buffering, utterance endpointing, and multipart upload inside a blocking
  single-threaded FSM. Deferred to its own spec.

---

## 6. Rollback

| To undo | Do this |
|---|---|
| TTS back to cloud | Captive portal → `tts_provider` / `tts_proact` = `openai`. No reflash; `tts_api_key` was never overwritten. |
| TTS fully local | Set both to `melotts`. |
| Proxy load-on-demand | `git checkout main` on lobsterboy + restart. The retry only triggers on `model_load_error`, so it is inert otherwise. |
| Firmware | Reflash a prior build over USB (§4.2). |

---

## 7. Loose ends from this session

- The CoreS3 USB device is marked **`Shared`** in usbipd on Windows
  (harmless — COM9 still works normally). `usbipd unbind --busid 2-4` as
  administrator reverts it. Note the busid moved 2-3 → 2-4 across a replug.
- Staging files remain in `C:\Users\jarod\AppData\Local\Temp\jarvis-flash`
  (firmware images and helper scripts). The provisioning payload containing
  the API key was deleted immediately after use.

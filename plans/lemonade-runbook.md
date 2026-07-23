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

Device: `jarvis.local` / **192.168.1.104**. Lemonade: **192.168.1.118:13305**.

---

## 2. lobsterboy — WHAT NEEDS DOING

### 2.1 Return to `main` after PR #63 merges  ← the one real action item

lobsterboy is currently running the **PR branch**, not `main`. It was
checked out to deploy the load-on-demand fix before the PR merged.

```bash
ssh lobsterboy
cd ~/Project-Jarvis
git checkout main && git pull
sudo systemctl restart oc-personal.service
systemctl is-active oc-personal.service          # expect: active
curl -s localhost:8080/healthz | head -c 200     # expect: backend amd-halo:13305
```

Leaving it on the branch is not harmful, but it will silently diverge from
`main` on the next unrelated deploy.

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
  `OC_BACKEND_URL=http://amd-halo:13305` and
  `OC_PROXY_FORCE_MODEL=gpt-oss-120b-Q4_K_M`.
- **The Lemonade API key on lobsterboy is `OC_BACKEND_TOKEN`** in
  `/home/jarod7736/.config/oc-personal/secrets.env` (same 64-char value as
  1Password `holdfast-lan` → "lemonade api key").

### 2.3 Nothing else to do on lobsterboy

The load-on-demand fix is deployed and verified. No unit changes, no new
secrets, no new services.

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

`max_models` is 2 per type (`llm`, `tts`, `transcription`). Qwen3-Coder and
gpt-oss now sit resident together; a third LLM request evicts by LRU. If
Claude Code suddenly slows down, check whether something evicted
Qwen3-Coder.

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

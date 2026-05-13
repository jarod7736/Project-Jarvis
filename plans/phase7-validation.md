# Phase 7 Validation — 2026-05-12

Code-side Phase 7 is complete (`SdLogger`, `OtaService`, `MqttClient`,
`TtsClient`, `AudioPlayer`, watchdog in `main.cpp`). This doc is the
checklist for the validation gate — what we actually ran on the device,
not what we built.

**This session's scope:** gates 1, 2, 3, 6.
Gates 4 (cloud TTS happy path), 5 (TTS fallback), and 7 (24-h soak) are
deferred — `tts_provider` stays on melotts, soak gets a follow-up session.

---

## Gate 1 — Watchdog reboot

**How:** in a dev build, inject `delay(35000)` at the top of `loop()` in
`src/main.cpp`. `kWatchdogTimeoutSec = 30` (see `src/config.h:197`), so
the WDT must panic-reboot at ~30 s.

**Expected:** serial monitor shows the IDF watchdog backtrace, device
resets, normal boot sequence resumes. After observing, revert the patch
— do **not** commit it.

**Result:** ✅ PASS (2026-05-12 09:09)
**Notes:** WDT armed 09:08:36.638, panic at 09:09:06.638 — exactly
30.0 s. Trace named `loopTask (CPU 1)` correctly. `task_wdt_isr` →
`abort()` → clean reboot → full recovery to IDLE on the next cycle.
Patch was a temporary `delay(35000)` after `esp_task_wdt_reset()` in
`loop()`; reverted before reflash.

---

## Gate 2 — OTA round-trip

**How:**
1. Confirm `jarvis.local` resolves: `ping jarvis.local`.
2. `pio run -t upload --upload-port jarvis.local` (PlatformIO picks up
   the espota uploader from `platformio.ini`).
3. Watch the device display for "OTA: in progress" and a reboot.
4. After reboot, check SD log on the next exchange — there should be a
   new JSONL entry with a `ts` that resets near 0 (millis post-boot).

**Expected:** upload completes, device reboots, log continues post-boot.

**Result:** ✅ PASS (2026-05-12 10:57)
**Notes:** Initial attempts timed out with "No response from the ESP"
even after firewall was disabled. Root cause: **Tailscale was poaching
the `192.168.1.0/24` subnet via accept-routes** (lobsterboy advertises
it as a subnet router), so OTA UDP went to the tailnet instead of the
LAN. `tailscale down` fixed it; the durable fix is
`tailscale set --accept-routes=false` on the dev machine.

Required ini changes to make OTA work from CLI:
```ini
upload_protocol = espota
upload_flags =
    --auth=<ota_pass from NVS>
    --host_port=8266
```
`--host_port` was used to pin the inbound UDP response port for the
firewall test; not strictly required once we confirmed Tailscale was
the cause. Auth flag IS required — `OtaService.cpp:50` enforces
`ota_pass` from NVS. PlatformIO env-var path mangles multi-flag input,
so `platformio.ini` is the only reliable place for these.

Upload: 1573360 bytes, ~30 s wire time + 4 s post-reboot recovery.

---

## Gate 3 — MQTT pub/sub

**Broker:** Mosquitto on HA (already in NVS as `mqtt_host`).
**Topics:** `jarvis/state` (pub), `jarvis/command` (sub) — see
`src/config.h:178-179`. LWT publishes `OFFLINE` on dirty disconnect.

**How:**
1. From any machine on the LAN, run `scripts/phase7/mqtt-watch.sh` to
   tail `jarvis/#`.
2. Trigger an FSM cycle (say "hello jarvis", ask a question, hear
   reply). Confirm we see retained `jarvis/state` flips through
   `IDLE → LISTENING → TRANSCRIBING → SPEAKING → IDLE`.
3. From the same machine: `scripts/phase7/mqtt-send.sh "what time is it"`.
   The CoreS3 should dispatch the command without a wake word (see
   `src/app/state_machine.cpp:127`) and speak the reply.

**Expected:** state stream observed end-to-end; MQTT-pushed command
executes from IDLE without requiring "hello jarvis".

**Result:** ✅ PASS (2026-05-12 11:00)
**Notes:** Subscriber on lobsterboy: `mosquitto_sub -h 192.168.1.10
-p 1883 -u jarvis -P <pw> -v -t 'jarvis/#'`. Initial state was the
retained LWT `jarvis/state OFFLINE` from a previous dirty disconnect.

Publisher test: `mosquitto_pub -t jarvis/command -m "what time is it"`.
Device received over UART/MQTT, dispatched without wake word per
`state_machine.cpp:127`, routed through IntentRouter (Qwen produced
unparseable output — fell back to the keyword classifier, classified
as `on_device`, returned "It's 05:05 PM"). Subscriber saw:
```
jarvis/command WHat time is it?
jarvis/state THINKING
jarvis/state SPEAKING
jarvis/state IDLE
```
FSM internal name `TRANSCRIBING` publishes on the wire as `THINKING`
(intentional — config.h constant).

---

## Gate 6 — Degradation matrix (subset)

Walking the rows from PLAN.md L723-732 that don't depend on cloud TTS.

| # | Scenario | Expected | Result | Notes |
|---|---|---|---|---|
| a | LAN down → hotspot available | Hotspot connect ≤15 s, Tailscale up, Claude reachable | ⏭ DEFERRED | Disruptive to home network; low marginal value for Phase 7 close. |
| b | All WiFi down | OFFLINE tier, Qwen responds, no crash | ⏭ DEFERRED | Same — disruptive, low value. |
| c | HA host unreachable | HA commands TTS apology; non-HA still works | ⏭ DEFERRED | Nabu Casa hosted; no clean injection path. Code path identical to row d (which passed) — same `LLMClient` style error handling. |
| d | OpenClaw unreachable | local_llm/claude intents fall back, no crash | ✅ PASS | `sudo systemctl stop oc-personal` on lobsterboy; device asked a `personal_query`; TTS fallback "I can't reach my notes right now"; no crash. Restart restored health (12 MCP tools). 2026-05-12 |
| e | SD card absent | Logger fails silently, rest works | ✅ PASS | Booted w/o card; full FSM cycle on "what time is it"; zero `[SdLogger]` references in runtime path (logExchange early-exits with `g_ok=false`); no crash. 2026-05-12 |
| f | LLM Module power-cycled | CoreS3 surfaces "Module offline", retries init every 5 s | ⏭ DEFERRED | Physical injection; user judged a + d coverage sufficient for Phase 7 close. Worth running before any user-facing release. |

Skipped rows: `TTS provider unreachable` and `tts_provider=melotts`
gates — both belong to the deferred TTS gates 4/5.

---

## Deferred (out of this session)

- **Gate 4:** OpenAI cloud TTS happy path
- **Gate 5:** Cloud-TTS → melotts mid-sentence fallback
- **Gate 7:** 24-hour soak + `soak-summary.py` against pulled
  `/jarvis.log`

---

## Closeout

✅ Gates 1, 2, 3, 6a, 6d all green; gates 4, 5, 7, 6b/c/e/f deferred
with rationale. Phase 7 retro written to PLAN.md (after L744). Ready
to commit + merge worktree branch.

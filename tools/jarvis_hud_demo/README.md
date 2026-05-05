# Jarvis HUD Demo

Visual prototype of the J.A.R.V.I.S. concentric-ring audio HUD for the M5Stack
CoreS3. Runs entirely with simulated audio so you can validate the look-and-feel
on real hardware before the ASR / TTS pipeline exists.

## What it shows

A 240x240 HUD on the left, an 80x240 status bar on the right.

The HUD has five concentric layers:

| Layer | Behavior                                              | Audio-reactive? |
|-------|-------------------------------------------------------|-----------------|
| Outer | Rotating tick marks, ~8s per rev                      | No (decorative) |
| FFT   | 48 radial bars, length = magnitude                    | YES             |
| Amp   | Sweep arc, fill = overall amplitude                   | YES             |
| Data  | Counter-rotating segments, ~5s per rev                | No (decorative) |
| Core  | Inner disc + ring, radius modulated by bass           | YES             |

The status bar shows clock, current state, network tier, intelligence tier,
HA status, simulated battery, and a vertical VU meter.

## State demo cycle

The demo auto-cycles through every state so you can preview each:

- **IDLE**       (4s)  - dim cyan, gentle ambient motion
- **LISTENING**  (5s)  - bright cyan, FFT reacts to simulated mic input
- **THINKING**   (3.5s) - amber, FFT mostly frozen, slow pulse
- **SPEAKING**   (6s)  - teal, full HUD active with simulated TTS

**Tap the touchscreen** to advance to the next state immediately.

## Requirements

- M5Stack CoreS3 (ESP32-S3)
- Arduino IDE 2.x or PlatformIO
- M5Stack board package installed (URL in Preferences:
  `https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json`)
- Library: **M5Unified** (Library Manager), pulls in M5GFX automatically

## Build & flash (Arduino IDE)

1. Tools -> Board -> M5Stack -> **M5CoreS3**
2. Tools -> USB CDC On Boot -> **Enabled** (so Serial works over USB-C)
3. Tools -> PSRAM -> **OPI PSRAM** (sprites are allocated in PSRAM)
4. Open `jarvis_hud_demo.ino`
5. Select the correct port, click Upload

## Performance notes

- Static decoration is rendered once per state into `bgSprite` and blitted
  fresh each frame, so we only redraw the dynamic rings.
- Sprites live in PSRAM (240x240x2 bytes = 115 KB each, three of them).
- Target frame rate ~30 fps; status bar redraws at ~5 Hz to save cycles.
- All trig is single-precision float; ESP32-S3 has hardware FPU so this is
  fine. If frame rate dips, pre-compute sin/cos lookup tables for the FFT
  bar angles.

## Wiring this to real audio (next step)

When the LLM Module arrives and you have I2S audio flowing, replace
`updateSimulatedAudio()` with:

1. An I2S read from the TTS or mic stream into a 256-sample buffer
2. `dsps_fft2r_fc32` (ESP-DSP) on that buffer
3. Map magnitude bins to `fftLevels[]` (log-scale binning recommended -
   group higher bins together, since they're perceptually less interesting)
4. Compute `ampSmoothed` from RMS of the time-domain buffer
5. Compute `bassSmoothed` from sum of the lowest 4-8 bins

Run that on core 1 in a FreeRTOS task; keep the rendering on core 0.

## Customization

- Ring radii: tweak the `R_*` constants near the top of the sketch
- Colors per state: edit `initPalettes()`
- Timing: `STATE_*_MS` constants
- Bar count: `FFT_BARS` (must divide evenly into 360 degrees for cleanest
  look; 48, 60, 72 all work well)


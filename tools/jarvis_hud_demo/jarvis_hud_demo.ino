// =============================================================================
// Jarvis HUD Demo - M5Stack CoreS3
// =============================================================================
// A standalone visual prototype of the J.A.R.V.I.S. concentric-ring audio HUD.
// Runs with simulated audio data so you can validate the look-and-feel before
// the real ASR/TTS pipeline is wired in.
//
// Layout: 320x240 display, split into:
//   - Left:  240x240 HUD region (concentric rings, FFT bars, core)
//   - Right:  80x240 status bar (clock, WiFi tier, intel tier, state label)
//
// State machine cycles through IDLE -> LISTENING -> THINKING -> SPEAKING
// every few seconds so you can preview every visual state.
//
// Dependencies:
//   - M5Unified  (Library Manager: "M5Unified" by M5Stack)
//   - M5GFX      (pulled in transitively by M5Unified)
//
// Board: "M5Stack-CoreS3" via M5Stack board package.
// =============================================================================

#include <M5Unified.h>
#include <math.h>

// ---- Display geometry ------------------------------------------------------
static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;
static constexpr int HUD_SIZE = 240;            // square HUD region
static constexpr int STATUS_X = HUD_SIZE;       // 240..319
static constexpr int STATUS_W = SCREEN_W - HUD_SIZE;  // 80
static constexpr int CX = HUD_SIZE / 2;         // HUD center x (in HUD sprite)
static constexpr int CY = HUD_SIZE / 2;         // HUD center y

// ---- Ring radii (tuned for 240x240) ----------------------------------------
static constexpr int R_OUTER_TICKS   = 116;  // ring 1: rotating tick marks
static constexpr int R_FFT_OUTER     = 104;  // ring 2: FFT bars outer edge
static constexpr int R_FFT_INNER     = 78;   // ring 2: FFT bars inner edge
static constexpr int R_AMP_ARC       = 70;   // ring 3: amplitude sweep arc
static constexpr int R_DATA_SEGS     = 60;   // ring 4: counter-rotating segs
static constexpr int R_CORE_OUTER    = 44;   // ring 5: pulsing core ring
static constexpr int R_CORE_INNER    = 36;   // inner edge of core ring
static constexpr int R_LABEL         = 30;   // text label region

// ---- FFT visualisation -----------------------------------------------------
static constexpr int FFT_BARS = 48;          // bars around the ring

// ---- Simulated audio + state cycling --------------------------------------
static constexpr uint32_t STATE_IDLE_MS      = 4000;
static constexpr uint32_t STATE_LISTENING_MS = 5000;
static constexpr uint32_t STATE_THINKING_MS  = 3500;
static constexpr uint32_t STATE_SPEAKING_MS  = 6000;

enum JarvisState : uint8_t {
  STATE_IDLE = 0,
  STATE_LISTENING,
  STATE_THINKING,
  STATE_SPEAKING,
  STATE_COUNT
};

static const char* STATE_NAMES[STATE_COUNT] = {
  "IDLE", "LISTEN", "THINK", "SPEAK"
};

// Color palette per state (RGB565). Cyan family for idle/listen/speak,
// amber for thinking. Glow is a darker variant of primary.
struct StatePalette {
  uint16_t primary;   // bright accent
  uint16_t glow;      // dim accent for "glow" double-stroke
  uint16_t dim;       // very dim version for static decoration
  uint16_t bg;        // background fill
  uint16_t text;      // label text color
};

static StatePalette PALETTES[STATE_COUNT];

static void initPalettes() {
  // Helper: M5GFX::color565 packs RGB into RGB565
  // IDLE - soft cyan, low energy
  PALETTES[STATE_IDLE]      = { M5.Display.color565(  0, 180, 200),
                                M5.Display.color565(  0,  90, 110),
                                M5.Display.color565(  0,  45,  60),
                                M5.Display.color565(  2,   8,  16),
                                M5.Display.color565(180, 230, 240) };
  // LISTENING - bright cyan
  PALETTES[STATE_LISTENING] = { M5.Display.color565(  0, 230, 255),
                                M5.Display.color565(  0, 130, 160),
                                M5.Display.color565(  0,  55,  75),
                                M5.Display.color565(  2,   8,  16),
                                M5.Display.color565(220, 245, 255) };
  // THINKING - amber
  PALETTES[STATE_THINKING]  = { M5.Display.color565(255, 170,  40),
                                M5.Display.color565(150,  90,  10),
                                M5.Display.color565( 70,  40,   0),
                                M5.Display.color565( 16,   8,   2),
                                M5.Display.color565(255, 220, 160) };
  // SPEAKING - teal/cyan as in the reference image
  PALETTES[STATE_SPEAKING]  = { M5.Display.color565(  0, 220, 230),
                                M5.Display.color565(  0, 120, 140),
                                M5.Display.color565(  0,  50,  70),
                                M5.Display.color565(  2,   8,  16),
                                M5.Display.color565(200, 245, 250) };
}

// ---- Sprites ---------------------------------------------------------------
// hudSprite: full 240x240 frame we composite each tick, then push to display.
// bgSprite : pre-rendered static decoration we blit as the starting frame.
//            Re-rendered when state changes (because colors change).
// statusSprite: 80x240 right column, redrawn ~2 Hz.
static M5Canvas hudSprite(&M5.Display);
static M5Canvas bgSprite(&M5.Display);
static M5Canvas statusSprite(&M5.Display);

// ---- State tracking --------------------------------------------------------
static JarvisState currentState = STATE_IDLE;
static uint32_t stateEnteredMs = 0;
static uint32_t lastStatusDrawMs = 0;

// Simulated FFT bin levels [0..1]
static float fftLevels[FFT_BARS] = {0};
// Smoothed overall amplitude [0..1]
static float ampSmoothed = 0.0f;
// Smoothed bass level (drives core pulse)
static float bassSmoothed = 0.0f;

// =============================================================================
// Static decoration: rendered once per state into bgSprite
// =============================================================================
static void drawStaticDecoration(M5Canvas& s, const StatePalette& p) {
  s.fillSprite(p.bg);

  // Faint vignette / radial darkening - skipped to save cycles, bg color is
  // already very dark navy.

  // Outermost ring: thin circle outline
  s.drawCircle(CX, CY, R_OUTER_TICKS + 4, p.dim);
  s.drawCircle(CX, CY, R_OUTER_TICKS + 6, p.dim);

  // Decorative small squares scattered along outer ring
  // (these are static, not the rotating ticks)
  for (int i = 0; i < 36; i++) {
    if (i % 5 == 0) continue;  // gaps
    float a = (i / 36.0f) * 2.0f * M_PI;
    int x = CX + (int)((R_OUTER_TICKS + 10) * cosf(a));
    int y = CY + (int)((R_OUTER_TICKS + 10) * sinf(a));
    s.fillRect(x - 1, y - 1, 2, 2, p.dim);
  }

  // Mid ring boundary circles (between FFT and amp arc)
  s.drawCircle(CX, CY, R_FFT_OUTER + 2, p.dim);
  s.drawCircle(CX, CY, R_FFT_INNER - 2, p.dim);

  // "Data" segments ring - small rectangles around R_DATA_SEGS, static
  for (int i = 0; i < 60; i++) {
    if ((i / 3) % 4 == 3) continue;  // create rhythmic gaps
    float a = (i / 60.0f) * 2.0f * M_PI;
    int rOuter = R_DATA_SEGS + 4;
    int rInner = R_DATA_SEGS;
    int x1 = CX + (int)(rInner * cosf(a));
    int y1 = CY + (int)(rInner * sinf(a));
    int x2 = CX + (int)(rOuter * cosf(a));
    int y2 = CY + (int)(rOuter * sinf(a));
    s.drawLine(x1, y1, x2, y2, p.dim);
  }

  // Inner ring boundary
  s.drawCircle(CX, CY, R_DATA_SEGS - 4, p.dim);

  // Core ring boundaries
  s.drawCircle(CX, CY, R_CORE_OUTER, p.glow);
  s.drawCircle(CX, CY, R_CORE_INNER, p.glow);
}

// =============================================================================
// Dynamic frame: rings that change every tick
// =============================================================================

// Helper - draw an arc segment as a series of short lines (M5GFX has
// fillArc but the line approach gives us better control over thickness).
static void drawArcThick(M5Canvas& s, int cx, int cy, int radius,
                         float startRad, float endRad, int thickness,
                         uint16_t color) {
  // Approx 1 line per 2 degrees of arc, scales with radius for smoothness
  float span = endRad - startRad;
  int steps = (int)(fabsf(span) * radius / 2.0f);
  if (steps < 2) steps = 2;
  for (int i = 0; i < steps; i++) {
    float a = startRad + span * (i / (float)steps);
    int xo = cx + (int)((radius + thickness / 2) * cosf(a));
    int yo = cy + (int)((radius + thickness / 2) * sinf(a));
    int xi = cx + (int)((radius - thickness / 2) * cosf(a));
    int yi = cy + (int)((radius - thickness / 2) * sinf(a));
    s.drawLine(xo, yo, xi, yi, color);
  }
}

// Outer ring: rotating tick marks
static void drawOuterRotatingTicks(M5Canvas& s, const StatePalette& p,
                                   float rotationRad) {
  const int N = 72;
  for (int i = 0; i < N; i++) {
    // Pattern: most ticks dim, a few bright clusters
    int cluster = i % 12;
    bool bright = (cluster < 4);
    if (cluster >= 7 && cluster < 9) continue;  // gaps
    uint16_t c = bright ? p.primary : p.glow;
    float a = rotationRad + (i / (float)N) * 2.0f * M_PI;
    int len = bright ? 8 : 4;
    int rIn = R_OUTER_TICKS - len / 2;
    int rOut = R_OUTER_TICKS + len / 2;
    int x1 = CX + (int)(rIn * cosf(a));
    int y1 = CY + (int)(rIn * sinf(a));
    int x2 = CX + (int)(rOut * cosf(a));
    int y2 = CY + (int)(rOut * sinf(a));
    s.drawLine(x1, y1, x2, y2, c);
  }
}

// FFT bar ring: radial bars whose length encodes magnitude
static void drawFFTRing(M5Canvas& s, const StatePalette& p) {
  const float startA = -M_PI / 2.0f;  // start at top
  for (int i = 0; i < FFT_BARS; i++) {
    float a = startA + (i / (float)FFT_BARS) * 2.0f * M_PI;
    float mag = fftLevels[i];  // 0..1
    if (mag < 0.02f) mag = 0.02f;
    int barLen = (int)(mag * (R_FFT_OUTER - R_FFT_INNER));
    int rIn = R_FFT_INNER;
    int rOut = R_FFT_INNER + barLen;
    int x1 = CX + (int)(rIn * cosf(a));
    int y1 = CY + (int)(rIn * sinf(a));
    int x2 = CX + (int)(rOut * cosf(a));
    int y2 = CY + (int)(rOut * sinf(a));
    // Color tier by magnitude - low = dim, high = primary
    uint16_t c = (mag > 0.6f) ? p.primary
                : (mag > 0.3f) ? p.glow
                : p.dim;
    s.drawLine(x1, y1, x2, y2, c);
    // glow: redraw bright bars one pixel offset for thicker look
    if (mag > 0.6f) {
      float aOff = a + 0.5f / R_FFT_INNER;
      int x1b = CX + (int)(rIn * cosf(aOff));
      int y1b = CY + (int)(rIn * sinf(aOff));
      int x2b = CX + (int)(rOut * cosf(aOff));
      int y2b = CY + (int)(rOut * sinf(aOff));
      s.drawLine(x1b, y1b, x2b, y2b, p.glow);
    }
  }
}

// Amplitude sweep arc: 0..360 degrees mapped from amplitude
static void drawAmpArc(M5Canvas& s, const StatePalette& p, float rotation) {
  float fillFrac = ampSmoothed;
  if (fillFrac < 0.05f) fillFrac = 0.05f;
  float startA = rotation - M_PI / 2.0f;
  float endA   = startA + fillFrac * 2.0f * M_PI;
  // Glow underlay
  drawArcThick(s, CX, CY, R_AMP_ARC, startA, endA, 5, p.glow);
  // Bright top stroke
  drawArcThick(s, CX, CY, R_AMP_ARC, startA, endA, 2, p.primary);
}

// Counter-rotating "data" segments inner ring
static void drawCounterRotatingSegs(M5Canvas& s, const StatePalette& p,
                                    float rotationRad) {
  const int N = 24;
  for (int i = 0; i < N; i++) {
    if (i % 6 == 5) continue;
    float a = -rotationRad + (i / (float)N) * 2.0f * M_PI;
    int rIn = R_DATA_SEGS - 8;
    int rOut = R_DATA_SEGS - 4;
    int x1 = CX + (int)(rIn * cosf(a));
    int y1 = CY + (int)(rIn * sinf(a));
    int x2 = CX + (int)(rOut * cosf(a));
    int y2 = CY + (int)(rOut * sinf(a));
    s.drawLine(x1, y1, x2, y2, p.primary);
  }
}

// Pulsing core driven by bass amplitude
static void drawPulsingCore(M5Canvas& s, const StatePalette& p) {
  // Pulse radius modulation
  int rPulse = R_CORE_INNER + (int)(bassSmoothed * 4.0f);
  // Filled inner disc - dim
  s.fillCircle(CX, CY, rPulse - 2, p.dim);
  // Bright outer ring
  s.drawCircle(CX, CY, rPulse, p.primary);
  s.drawCircle(CX, CY, rPulse + 1, p.glow);
}

// Center label
static void drawCenterLabel(M5Canvas& s, const StatePalette& p,
                            const char* text) {
  s.setTextColor(p.text, p.bg);
  s.setTextDatum(middle_center);
  s.setTextSize(1);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.drawString(text, CX, CY);
}

// =============================================================================
// Status bar (right 80px column)
// =============================================================================
static void drawStatusBar(M5Canvas& s, const StatePalette& p,
                          JarvisState state, uint32_t now) {
  s.fillSprite(p.bg);

  // Top divider
  s.drawFastVLine(0, 0, SCREEN_H, p.dim);

  // --- Clock (HH:MM, simulated) ---
  uint32_t totalSec = now / 1000;
  int hh = (10 + (totalSec / 3600)) % 24;
  int mm = (totalSec / 60) % 60;
  char clockBuf[8];
  snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", hh, mm);
  s.setTextColor(p.text, p.bg);
  s.setTextDatum(top_center);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.drawString(clockBuf, STATUS_W / 2, 8);

  // --- State label ---
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextColor(p.primary, p.bg);
  s.drawString(STATE_NAMES[state], STATUS_W / 2, 44);

  // --- WiFi tier ---
  s.setFont(&fonts::Font0);
  s.setTextColor(p.text, p.bg);
  s.setTextDatum(top_left);
  s.drawString("NET", 6, 80);
  s.setTextColor(p.primary, p.bg);
  s.setTextDatum(top_right);
  s.drawString("HOME", STATUS_W - 6, 80);

  // --- Intelligence tier ---
  s.setTextColor(p.text, p.bg);
  s.setTextDatum(top_left);
  s.drawString("AI", 6, 96);
  s.setTextColor(p.primary, p.bg);
  s.setTextDatum(top_right);
  const char* tier = (state == STATE_THINKING) ? "CLAUDE"
                   : (state == STATE_SPEAKING) ? "OPENCLAW"
                   : "QWEN";
  s.drawString(tier, STATUS_W - 6, 96);

  // --- HA status ---
  s.setTextColor(p.text, p.bg);
  s.setTextDatum(top_left);
  s.drawString("HA", 6, 112);
  s.setTextColor(p.primary, p.bg);
  s.setTextDatum(top_right);
  s.drawString("OK", STATUS_W - 6, 112);

  // --- Battery (simulated) ---
  s.setTextColor(p.text, p.bg);
  s.setTextDatum(top_left);
  s.drawString("BAT", 6, 128);
  s.setTextColor(p.primary, p.bg);
  s.setTextDatum(top_right);
  s.drawString("87%", STATUS_W - 6, 128);

  // --- Audio meter (vertical bar) at bottom ---
  int meterX = 10;
  int meterY = 160;
  int meterW = STATUS_W - 20;
  int meterH = 60;
  s.drawRect(meterX, meterY, meterW, meterH, p.dim);
  int fill = (int)(ampSmoothed * (meterH - 2));
  if (fill > 0) {
    s.fillRect(meterX + 1, meterY + meterH - 1 - fill,
               meterW - 2, fill, p.primary);
  }

  // Bottom label
  s.setFont(&fonts::Font0);
  s.setTextColor(p.text, p.bg);
  s.setTextDatum(bottom_center);
  s.drawString("J.A.R.V.I.S", STATUS_W / 2, SCREEN_H - 4);
}

// =============================================================================
// Audio simulation
// =============================================================================
// Generate believable FFT levels and amplitude for each state.
// In production this is replaced by ESP-DSP FFT on the I2S TTS stream.
static void updateSimulatedAudio(JarvisState state, uint32_t now) {
  float t = now / 1000.0f;
  float targetAmp = 0.0f;
  float speechEnvelope = 0.0f;

  switch (state) {
    case STATE_IDLE:
      // Tiny ambient noise, occasional shimmer
      targetAmp = 0.05f + 0.03f * sinf(t * 0.7f);
      break;

    case STATE_LISTENING:
      // Mid-level, more reactive - simulates user speaking into mic
      // Speech envelope: bursts of activity with quiet gaps
      speechEnvelope = 0.5f + 0.5f * sinf(t * 3.1f);
      speechEnvelope *= (sinf(t * 0.9f) > -0.3f) ? 1.0f : 0.2f;
      targetAmp = 0.35f * speechEnvelope + 0.05f;
      break;

    case STATE_THINKING:
      // Frozen-ish, slow pulse only - FFT bars hold steady at low values
      targetAmp = 0.15f + 0.10f * sinf(t * 1.5f);
      break;

    case STATE_SPEAKING:
      // High energy, varied - simulates TTS playback
      speechEnvelope = 0.6f + 0.4f * sinf(t * 2.3f);
      speechEnvelope *= (0.7f + 0.3f * sinf(t * 5.7f));
      targetAmp = 0.55f * speechEnvelope + 0.10f;
      break;

    default: break;
  }

  // Smooth amplitude with attack/release envelope
  float attack = 0.35f, release = 0.08f;
  float coef = (targetAmp > ampSmoothed) ? attack : release;
  ampSmoothed += (targetAmp - ampSmoothed) * coef;
  if (ampSmoothed < 0) ampSmoothed = 0;
  if (ampSmoothed > 1) ampSmoothed = 1;

  // Bass (drives core pulse): low-freq component, slower envelope
  float targetBass = targetAmp * (0.6f + 0.4f * sinf(t * 1.7f));
  bassSmoothed += (targetBass - bassSmoothed) * 0.15f;
  if (bassSmoothed < 0) bassSmoothed = 0;
  if (bassSmoothed > 1) bassSmoothed = 1;

  // Per-bar FFT levels
  for (int i = 0; i < FFT_BARS; i++) {
    // Fake spectrum: low bins louder than high bins, modulated per-bar
    float binFreq = i / (float)FFT_BARS;
    float falloff = expf(-binFreq * 1.5f);  // bass-heavy
    // Each bar has its own oscillator for liveliness
    float jitter = 0.5f + 0.5f * sinf(t * (2.0f + i * 0.13f) + i * 0.7f);
    float target = ampSmoothed * falloff * jitter;
    // Add some sparkle in mid/high during SPEAKING
    if (state == STATE_SPEAKING && i > FFT_BARS / 2) {
      target += 0.15f * ampSmoothed *
                (0.5f + 0.5f * sinf(t * 7.0f + i * 1.1f));
    }
    if (target < 0) target = 0;
    if (target > 1) target = 1;
    // Per-bar smoothing
    float c = (target > fftLevels[i]) ? 0.5f : 0.15f;
    fftLevels[i] += (target - fftLevels[i]) * c;
  }
}

// =============================================================================
// State machine
// =============================================================================
static uint32_t durationFor(JarvisState s) {
  switch (s) {
    case STATE_IDLE: return STATE_IDLE_MS;
    case STATE_LISTENING: return STATE_LISTENING_MS;
    case STATE_THINKING: return STATE_THINKING_MS;
    case STATE_SPEAKING: return STATE_SPEAKING_MS;
    default: return 3000;
  }
}

static void advanceStateIfNeeded(uint32_t now) {
  if (now - stateEnteredMs >= durationFor(currentState)) {
    currentState = (JarvisState)((currentState + 1) % STATE_COUNT);
    stateEnteredMs = now;
    // Re-render the static decoration with new palette
    drawStaticDecoration(bgSprite, PALETTES[currentState]);
  }
}

// Center label per state (in production, transcript / response text)
static const char* labelFor(JarvisState s) {
  switch (s) {
    case STATE_IDLE: return "J.A.R.V.I.S";
    case STATE_LISTENING: return "LISTENING";
    case STATE_THINKING: return "THINKING";
    case STATE_SPEAKING: return "SPEAKING";
    default: return "";
  }
}

// =============================================================================
// Setup / Loop
// =============================================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);  // landscape
  M5.Display.fillScreen(TFT_BLACK);

  initPalettes();

  // Allocate sprites
  hudSprite.setColorDepth(16);
  hudSprite.createSprite(HUD_SIZE, HUD_SIZE);
  hudSprite.setPsram(true);

  bgSprite.setColorDepth(16);
  bgSprite.createSprite(HUD_SIZE, HUD_SIZE);
  bgSprite.setPsram(true);

  statusSprite.setColorDepth(16);
  statusSprite.createSprite(STATUS_W, SCREEN_H);
  statusSprite.setPsram(true);

  drawStaticDecoration(bgSprite, PALETTES[currentState]);
  stateEnteredMs = millis();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  // Touch to manually skip to next state (handy for demo)
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    currentState = (JarvisState)((currentState + 1) % STATE_COUNT);
    stateEnteredMs = now;
    drawStaticDecoration(bgSprite, PALETTES[currentState]);
  }

  advanceStateIfNeeded(now);
  updateSimulatedAudio(currentState, now);

  const StatePalette& pal = PALETTES[currentState];

  // Compose HUD frame: copy static bg, then draw dynamic layers
  bgSprite.pushSprite(&hudSprite, 0, 0);

  // Rotation angles (slow outer, faster counter inner)
  float outerRot = (now / 8000.0f) * 2.0f * M_PI;        // ~8s/rev
  float innerRot = -(now / 5000.0f) * 2.0f * M_PI;       // ~5s/rev opposite
  float ampRot   = (now / 12000.0f) * 2.0f * M_PI;       // amp arc drift

  drawOuterRotatingTicks(hudSprite, pal, outerRot);
  drawFFTRing(hudSprite, pal);
  drawAmpArc(hudSprite, pal, ampRot);
  drawCounterRotatingSegs(hudSprite, pal, innerRot);
  drawPulsingCore(hudSprite, pal);
  drawCenterLabel(hudSprite, pal, labelFor(currentState));

  // Push HUD to display
  hudSprite.pushSprite(0, 0);

  // Status bar at ~5 Hz to save CPU
  if (now - lastStatusDrawMs > 200) {
    drawStatusBar(statusSprite, pal, currentState, now);
    statusSprite.pushSprite(STATUS_X, 0);
    lastStatusDrawMs = now;
  }

  // Frame pacing - target ~30 fps
  delay(16);
}

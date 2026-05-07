// Display.cpp — Jarvis on-device UI: Tri-Arc · Sonar · Phosphor port.
//
// Recreation of the design the user landed on in claude.ai/design — see
// docs/jarvis_particle_prototype.html and the JARVIS HUD bundle. The
// reference design is React/SVG; this is the firmware port using direct
// M5GFX draws on the CoreS3 320×240 panel.
//
// Implementation choices vs the React prototype:
//   - No sprite / no PSRAM. The previous particle-field port used a
//     32bpp full-screen sprite in PSRAM and the alpha-blend pass was
//     too heavy. Sonar is structurally lighter (a few rings + a few
//     waveform lines) so we draw straight to M5.Display and accept a
//     small amount of flicker during the per-frame redraw of the
//     central reactor band.
//   - Solid colors instead of alpha glow. M5GFX has no real per-pixel
//     blending on direct-to-LCD drawing, so each shape is a single
//     stroke or fill. Lines are integer-pixel weight (1-2 px).
//   - 24 radial waveform "spokes" instead of the prototype's 48 — keeps
//     the per-frame draw count down. At 320×240 this still reads as a
//     continuous radial fan.
//   - Frame cadence: ~12 FPS (kFrameMs = 80). Slow enough to not steal
//     time from the LLMModule UART poll + FSM tick + MQTT/OTA, fast
//     enough that the spin/ring/waveform animations look fluid.
//
// Public API matches the prior boxed-region UI so callers (state_machine,
// main loop, FSM) don't need any changes.

#include "Display.h"

#include <M5Unified.h>
#include <math.h>
#include <time.h>

namespace jarvis::hal {

// ── Layout ──────────────────────────────────────────────────────────────────
namespace {
constexpr int SCR_W = 320;
constexpr int SCR_H = 240;

// Top status bar — chrome left/right + battery/wifi icons
constexpr int CHROME_Y     = 4;
constexpr int CHROME_H     = 14;

// Central reactor band
constexpr int REACTOR_Y    = 22;
constexpr int REACTOR_H    = 188;
constexpr int REACTOR_CX   = SCR_W / 2;
constexpr int REACTOR_CY   = REACTOR_Y + REACTOR_H / 2 - 4;

// Bottom band (idle hint or transcript)
constexpr int BOTTOM_Y     = 212;
constexpr int BOTTOM_H     = 28;

// Battery indicator (top-right)
constexpr int BATT_BODY_W  = 18;
constexpr int BATT_BODY_H  = 10;
constexpr int BATT_TIP_W   = 2;
constexpr int BATT_TIP_H   = 4;
constexpr int BATT_TIP_X   = SCR_W - 6 - BATT_TIP_W;
constexpr int BATT_BODY_X  = BATT_TIP_X - BATT_BODY_W;
constexpr int BATT_BODY_Y  = CHROME_Y + (CHROME_H - BATT_BODY_H) / 2;
constexpr int BATT_TIP_Y   = CHROME_Y + (CHROME_H - BATT_TIP_H) / 2;

// Frame cadence — 12 FPS keeps the animation fluid without crowding the
// voice pipeline.
constexpr uint32_t kFrameMs = 80;

// ── Phosphor palette (from REACTOR_THEMES.phosphor in the bundle) ───────────
//   bg      #04060a  near-black background
//   bg2     #080d10  ring of slightly lighter dark behind the reactor
//   fg      #ffd388  warm amber text
//   dim     #7a5a2a  brown for outer frame, chrome subtitle
//   primary #ffaa2a  amber — listening color
//   accent  #5dff8e  mint  — response color
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t COL_BG       = rgb565(0x04, 0x06, 0x0A);
const uint16_t COL_BG2      = rgb565(0x08, 0x0D, 0x10);
const uint16_t COL_FG       = rgb565(0xFF, 0xD3, 0x88);
const uint16_t COL_DIM      = rgb565(0x7A, 0x5A, 0x2A);
const uint16_t COL_PRIMARY  = rgb565(0xFF, 0xAA, 0x2A);
const uint16_t COL_ACCENT   = rgb565(0x5D, 0xFF, 0x8E);
const uint16_t COL_DANGER   = rgb565(0xFF, 0x4D, 0x4D);

// ── Cached state ────────────────────────────────────────────────────────────
DeviceState g_state          = DeviceState::IDLE;
uint32_t    g_last_frame_ms  = 0;
uint32_t    g_seed           = 0x1234ABCD;
float       g_mic_level      = 0.0f;

int    g_battery_level    = -1;
bool   g_battery_charging = false;
String g_wifi_tier        = "...";
int    g_wifi_rssi        = 0;
bool   g_ota_active       = false;

// Transcript / response text — populated by showTranscript / showResponse,
// reset on state transitions. The reference design splits these into a
// dim "▸ query" line and a bright response line; we follow the same
// pattern but render plain text since we don't have a typewriter effect
// budget on-device.
String  g_query_text;
String  g_response_text;

// ── PRNG (xorshift32) ───────────────────────────────────────────────────────
inline uint32_t xs32() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
inline float frand() { return (float)(xs32() & 0xFFFFFF) / (float)0xFFFFFF; }

// ── Drawing helpers ─────────────────────────────────────────────────────────

// State chrome string — top-right corner. Mirrors the reference design's
// "RX·CORE" / "IN·MIC1" / "NEURAL·LM" / "OUT·SPK" / "RDY" labels.
const char* stateChromeText(DeviceState s) {
    switch (s) {
        case DeviceState::LISTENING: return "IN-MIC1";
        case DeviceState::THINKING:  return "NEURAL-LM";
        case DeviceState::SPEAKING:  return "OUT-SPK";
        case DeviceState::ERROR:     return "ERR";
        case DeviceState::IDLE:
        default:                     return "RDY";
    }
}

// Battery icon + percentage in the top-right.
void drawBattery() {
    M5.Display.fillRect(BATT_BODY_X - 30, CHROME_Y, 30 + BATT_BODY_W + BATT_TIP_W + 2,
                        CHROME_H, COL_BG);
    // Body outline + tip
    M5.Display.drawRect(BATT_BODY_X, BATT_BODY_Y, BATT_BODY_W, BATT_BODY_H, COL_DIM);
    M5.Display.fillRect(BATT_TIP_X, BATT_TIP_Y, BATT_TIP_W, BATT_TIP_H, COL_DIM);
    // Fill
    int pct = g_battery_level;
    if (pct >= 0) {
        if (pct > 100) pct = 100;
        int bw = ((BATT_BODY_W - 4) * pct) / 100;
        if (bw > 0) {
            uint16_t col = (pct < 20) ? COL_DANGER : COL_PRIMARY;
            M5.Display.fillRect(BATT_BODY_X + 2, BATT_BODY_Y + 2, bw, BATT_BODY_H - 4, col);
        }
    }
    // Charging glyph (small "+" beside the battery)
    if (g_battery_charging) {
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(COL_ACCENT, COL_BG);
        M5.Display.setCursor(BATT_BODY_X - 10, CHROME_Y + 3);
        M5.Display.print("+");
    }
}

// WiFi connection arc + status. Drawn with two arcs and a center dot —
// matches the prototype's status-bar wifi glyph (curved bars), not the
// 3-bar bargraph the previous boxed UI used.
void drawWifi() {
    constexpr int WX = SCR_W - 88, WY = CHROME_Y + 7;
    M5.Display.fillRect(WX - 8, CHROME_Y, 18, CHROME_H, COL_BG);

    bool offline    = g_wifi_tier.equalsIgnoreCase("OFF");
    bool connecting = g_wifi_tier.equals("...") || g_wifi_tier.length() == 0;
    uint16_t col = offline ? COL_DANGER : (connecting ? COL_DIM : COL_PRIMARY);

    // Outer arc (broader curve)
    M5.Display.drawArc(WX, WY, 7, 5, 200, 340, col);
    // Inner arc
    M5.Display.drawArc(WX, WY, 4, 3, 200, 340, col);
    // Center dot
    M5.Display.fillCircle(WX, WY - 1, 1, col);
}

// Top-left chrome: "RX-CORE" left, state label right.
// Right side also reserves room for OTA badge if active.
void drawChrome() {
    M5.Display.fillRect(0, 0, SCR_W, CHROME_H + 6, COL_BG);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_DIM, COL_BG);
    M5.Display.setCursor(6, CHROME_Y + 3);
    M5.Display.print("RX-CORE");

    // State chrome — mid-left of the bar
    M5.Display.setTextColor(COL_PRIMARY, COL_BG);
    M5.Display.setCursor(SCR_W / 2 - 24, CHROME_Y + 3);
    M5.Display.print(stateChromeText(g_state));

    // Optional OTA badge to the left of the wifi+battery cluster
    if (g_ota_active) {
        M5.Display.setTextColor(COL_ACCENT, COL_BG);
        M5.Display.setCursor(SCR_W - 110, CHROME_Y + 3);
        M5.Display.print("OTA");
    }

    drawWifi();
    drawBattery();
}

// Mic level synth — pumps the radial waveform. We don't have a real
// audio level signal in this codebase yet (the Module-LLM ASR runs
// off-MCU on the Axera SoC), so we synthesize a plausible amplitude
// envelope from time. Listening: jittery + slow undulation. Speaking:
// faster + more sinusoidal. Idle/Thinking: zero.
void synthMicLevel(float t) {
    if (g_state == DeviceState::LISTENING) {
        g_mic_level = 0.3f + 0.6f * (0.5f + 0.5f * sinf(t * 4.0f + sinf(t * 11.0f) * 2.0f));
    } else if (g_state == DeviceState::SPEAKING) {
        g_mic_level = 0.4f + 0.5f * (0.5f + 0.5f * sinf(t * 7.0f + sinf(t * 13.0f) * 1.5f));
    } else {
        g_mic_level = 0.0f;
    }
}

// ── Reactor render ──────────────────────────────────────────────────────────
// The bulk of the per-frame work. Draws (in order, back-to-front):
//   1. Background fill
//   2. Outer frame circle
//   3. Two rotating orbital arcs (one CW, one CCW)
//   4. Inbound collapsing rings (LISTENING) or outbound expanding rings (SPEAKING)
//   5. Radial waveform spokes (LISTENING / SPEAKING only)
//   6. Core circle (outline + filled center, color and size by state)
void drawReactor(float t) {
    // Clear the reactor band.
    M5.Display.fillRect(0, REACTOR_Y, SCR_W, REACTOR_H, COL_BG);

    const int cx = REACTOR_CX;
    const int cy = REACTOR_CY;

    // 1. Outer frame circle (R≈86 — fits inside the band with a bit of margin)
    M5.Display.drawCircle(cx, cy, 86, COL_DIM);

    // 2. Rotating orbital arcs.
    // Outer arc — primary color, 270° span, slow rotation (full turn = 16s in idle,
    // 3s in thinking — we sweep faster while thinking like the prototype).
    float spin_period_s = (g_state == DeviceState::THINKING) ? 3.0f : 16.0f;
    float spin_phase    = fmodf(t / spin_period_s, 1.0f);   // 0..1
    int   outer_start   = (int)(spin_phase * 360.0f) % 360;
    int   outer_end     = (outer_start + 270) % 360;
    if (outer_end < outer_start) {
        // drawArc handles wrap natively when end < start by rendering
        // [start..360) ∪ [0..end). LovyanGFX accepts either ordering.
    }
    // drawArc(x, y, outer_r, inner_r, start_deg, end_deg, color)
    M5.Display.drawArc(cx, cy, 76, 75, outer_start, outer_end, COL_PRIMARY);
    // Knob at the start of the outer arc.
    {
        float a = outer_start * (M_PI / 180.0f);
        int   px = cx + (int)(cosf(a) * 76);
        int   py = cy + (int)(sinf(a) * 76);
        M5.Display.fillCircle(px, py, 2, COL_PRIMARY);
    }

    // Inner arc — accent color, counter-rotating, 180° span.
    float spin_period_s2 = (g_state == DeviceState::THINKING) ? 2.4f : 11.0f;
    float spin_phase2    = fmodf(t / spin_period_s2, 1.0f);
    int   inner_start    = (360 - (int)(spin_phase2 * 360.0f)) % 360;
    int   inner_end      = (inner_start + 180) % 360;
    M5.Display.drawArc(cx, cy, 60, 59, inner_start, inner_end, COL_ACCENT);

    // 3. State-specific ring animation.
    if (g_state == DeviceState::LISTENING) {
        // Three inbound collapsing rings, staggered. Each ring runs a 1.6s
        // cycle: scale drops from 76 down to ~19. Phase offsets are 0,
        // 0.533, 1.066s.
        const float period = 1.6f;
        for (int i = 0; i < 3; ++i) {
            float phase = fmodf(t - i * 0.5f, period) / period;
            if (phase < 0) phase += 1.0f;
            float r = 76.0f - phase * (76.0f - 19.0f);
            // Fade out as it reaches the core (last 30% of cycle)
            if (phase > 0.7f) continue;
            int ri = (int)r;
            if (ri > 19) M5.Display.drawCircle(cx, cy, ri, COL_PRIMARY);
        }
    } else if (g_state == DeviceState::SPEAKING) {
        // Outbound ripples — three rings expanding outward. 2.0s cycle,
        // staggered, fade as they reach the outer frame.
        const float period = 2.0f;
        for (int i = 0; i < 3; ++i) {
            float phase = fmodf(t - i * 0.65f, period) / period;
            if (phase < 0) phase += 1.0f;
            float r = 24.0f + phase * (84.0f - 24.0f);
            // Fade by skipping the last 20% (couldn't draw partial alpha here)
            if (phase > 0.8f) continue;
            int ri = (int)r;
            if (ri < 84) M5.Display.drawCircle(cx, cy, ri, COL_ACCENT);
        }
    }

    // 4. Radial waveform spokes — only during LISTENING / SPEAKING.
    if (g_state == DeviceState::LISTENING || g_state == DeviceState::SPEAKING) {
        constexpr int kSpokes = 24;
        for (int i = 0; i < kSpokes; ++i) {
            // Per-spoke amplitude — combines the synthesized mic level with
            // a per-spoke pseudo-random wobble for that "live" radial fan look.
            float jitter = 0.5f + 0.5f * sinf(t * (3.0f + (i & 7) * 0.4f) + i);
            float amp    = g_mic_level * jitter;
            float a      = (float)i / kSpokes * (2.0f * M_PI);
            float ca     = cosf(a), sa = sinf(a);
            if (g_state == DeviceState::LISTENING) {
                // Inbound: spokes start at the outer ring and point inward,
                // length grows with amplitude. Primary color.
                int r2 = 50;
                int r1 = 50 - (int)(amp * 14.0f) - 3;
                M5.Display.drawLine(cx + (int)(ca * r1), cy + (int)(sa * r1),
                                    cx + (int)(ca * r2), cy + (int)(sa * r2),
                                    COL_PRIMARY);
            } else {
                // Outbound: spokes radiate from the core outward. Accent color.
                int r1 = 22;
                int r2 = 22 + (int)(amp * 18.0f) + 3;
                M5.Display.drawLine(cx + (int)(ca * r1), cy + (int)(sa * r1),
                                    cx + (int)(ca * r2), cy + (int)(sa * r2),
                                    COL_ACCENT);
            }
        }
    }

    // 5. Core. Outline circle + filled center; size + color by state.
    bool resp = (g_state == DeviceState::SPEAKING);
    bool list = (g_state == DeviceState::LISTENING);
    bool err  = (g_state == DeviceState::ERROR);
    uint16_t coreCol = err ? COL_DANGER : (resp ? COL_ACCENT : COL_PRIMARY);
    M5.Display.drawCircle(cx, cy, 20, coreCol);
    int innerR = resp ? 12 : (list ? 5 : 7);
    M5.Display.fillCircle(cx, cy, innerR, coreCol);
}

// ── Bottom band: idle hint or transcript ────────────────────────────────────
void drawBottom() {
    M5.Display.fillRect(0, BOTTOM_Y, SCR_W, BOTTOM_H, COL_BG);
    M5.Display.setTextSize(1);

    if (g_state == DeviceState::IDLE) {
        M5.Display.setTextColor(COL_DIM, COL_BG);
        const char* hint = "SAY \"HEY JARVIS\"";
        // Center horizontally — 16 chars × 6 px = 96px wide
        int w = (int)strlen(hint) * 6;
        M5.Display.setCursor((SCR_W - w) / 2, BOTTOM_Y + 12);
        M5.Display.print(hint);
        return;
    }

    if (g_state == DeviceState::THINKING || g_state == DeviceState::SPEAKING) {
        // Query line (dim chrome) on top, response (bright) below.
        if (g_query_text.length() > 0) {
            M5.Display.setTextColor(COL_DIM, COL_BG);
            M5.Display.setCursor(8, BOTTOM_Y + 2);
            M5.Display.print("> ");
            // Truncate query to fit one line (~50 chars at textSize=1)
            String q = g_query_text;
            if (q.length() > 48) q = q.substring(0, 47) + "..";
            M5.Display.print(q);
        }

        if (g_state == DeviceState::SPEAKING && g_response_text.length() > 0) {
            M5.Display.setTextColor(COL_FG, COL_BG);
            M5.Display.setCursor(8, BOTTOM_Y + 14);
            String r = g_response_text;
            if (r.length() > 48) r = r.substring(0, 47) + "..";
            M5.Display.print(r);
        } else if (g_state == DeviceState::THINKING) {
            M5.Display.setTextColor(COL_DIM, COL_BG);
            M5.Display.setCursor(8, BOTTOM_Y + 14);
            M5.Display.print("computing...");
        }
        return;
    }

    if (g_state == DeviceState::ERROR) {
        M5.Display.setTextColor(COL_DANGER, COL_BG);
        M5.Display.setCursor(8, BOTTOM_Y + 8);
        M5.Display.print("ERROR");
    }
}
}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────
void Display::begin() {
    M5.Display.setTextWrap(false);
    M5.Display.fillScreen(COL_BG);
    drawChrome();
    // Reactor + bottom paint on the first tickWaveform() — set last_frame_ms
    // to 0 so the first call paints immediately.
    g_last_frame_ms = 0;
}

void Display::setStatus(DeviceState state) {
    if (state != g_state) {
        g_state = state;
        // State change: clear transcript text on transitions back to idle,
        // and keep otherwise — showTranscript / showResponse repopulate.
        if (state == DeviceState::IDLE) {
            g_query_text    = "";
            g_response_text = "";
        }
        drawChrome();    // updates the state label immediately
        drawBottom();    // refreshes idle hint / transcript visibility
    }
}

void Display::tickWaveform() {
    uint32_t now = millis();
    if (now - g_last_frame_ms < kFrameMs) return;
    g_last_frame_ms = now;

    float t = now * 0.001f;
    synthMicLevel(t);
    drawReactor(t);
}

void Display::updateBattery(int level, bool charging) {
    g_battery_level    = level;
    g_battery_charging = charging;
    drawBattery();
}

void Display::showTranscript(const String& text) {
    g_query_text = text;
    drawBottom();
}

void Display::showResponse(const String& text) {
    g_response_text = text;
    drawBottom();
}

void Display::updateFooter(const String& tier, int rssi) {
    g_wifi_tier = tier;
    g_wifi_rssi = rssi;
    drawWifi();
}

void Display::setOtaActive(bool active) {
    if (g_ota_active == active) return;
    g_ota_active = active;
    drawChrome();
}

void Display::setBrightness(int v) {
    if (v < 10)  v = 10;
    if (v > 255) v = 255;
    M5.Display.setBrightness(static_cast<uint8_t>(v));
}

void Display::drawConfigScreen() {
    // Bypass the reactor entirely. Static screen, painted once on entry
    // to Config mode (ModeManager pauses the voice loop while up).
    M5.Display.fillScreen(rgb565(0x04, 0x06, 0x0A));
    M5.Display.setTextDatum(top_left);

    M5.Display.setTextColor(COL_PRIMARY);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(60, 20);
    M5.Display.print("CONFIG MODE");

    M5.Display.setTextColor(COL_FG);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(20, 70);
    M5.Display.print("Connect phone to WiFi:");

    M5.Display.setTextSize(2);
    M5.Display.setCursor(40, 100);
    M5.Display.setTextColor(COL_ACCENT);
    M5.Display.print("Jarvis-Setup");

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_FG);
    M5.Display.setCursor(20, 150);
    M5.Display.print("Then open in browser:");
    M5.Display.setCursor(40, 170);
    M5.Display.setTextColor(COL_ACCENT);
    M5.Display.print("http://192.168.4.1");

    M5.Display.setTextColor(COL_DIM);
    M5.Display.setCursor(40, 220);
    M5.Display.print("Hold screen 2s to exit");
}

}  // namespace jarvis::hal

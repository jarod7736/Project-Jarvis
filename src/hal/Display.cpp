#include "Display.h"

#include <M5Unified.h>
#include <time.h>

namespace jarvis::hal {

// ── Layout (px) ─────────────────────────────────────────────────────────────
// CoreS3 display: 320 × 240.  All Y values are top-of-region.
namespace {
    constexpr int SCR_W   = 320;

    constexpr int STATUS_Y = 0,   STATUS_H = 30;
    // 5px implicit gap (black) between status bar and transcript
    constexpr int TRANS_Y  = 35,  TRANS_H  = 60;
    // 5px implicit gap then 1px divider
    constexpr int DIV1_Y   = 100;
    constexpr int RESP_Y   = 101, RESP_H   = 109;
    constexpr int DIV2_Y   = 210;
    constexpr int FOOT_Y   = 211, FOOT_H   = 29;

    // Status-bar dot geometry
    constexpr int DOT_X  = 10;
    constexpr int DOT_Y  = STATUS_Y + STATUS_H / 2;  // vertically centred
    constexpr int DOT_R  = 6;
    constexpr int LBL_X  = DOT_X + DOT_R + 6;        // 4px gap after dot
    constexpr int LBL_Y  = STATUS_Y + (STATUS_H - 16) / 2;  // textSize=2 → 16px tall

    // Battery indicator (top-right of status bar). Body + tab + pct text.
    //   [ ##### ]| 87%   ← drawn right-to-left
    constexpr int BATT_BODY_W = 20;
    constexpr int BATT_BODY_H = 12;
    constexpr int BATT_TIP_W  = 2;
    constexpr int BATT_TIP_H  = 6;
    // Right edge of the tip sits 4px from screen edge.
    constexpr int BATT_TIP_X  = SCR_W - 4 - BATT_TIP_W;
    constexpr int BATT_BODY_X = BATT_TIP_X - BATT_BODY_W;
    constexpr int BATT_BODY_Y = STATUS_Y + (STATUS_H - BATT_BODY_H) / 2;
    constexpr int BATT_TIP_Y  = STATUS_Y + (STATUS_H - BATT_TIP_H) / 2;
    // Percentage text: 4 chars max ("100%" / "--%"), 6px each at textSize=1.
    constexpr int BATT_PCT_W  = 4 * 6;  // 24
    constexpr int BATT_PCT_X  = BATT_BODY_X - 4 - BATT_PCT_W;
    constexpr int BATT_PCT_Y  = STATUS_Y + (STATUS_H - 8) / 2;

    // SPEAKING-state waveform animation. A 30 px band at the bottom of
    // the response region, drawn as a continuous oscilloscope-style
    // waveform: many points across the width, connected by line
    // segments, each point jittered around the centerline. Looks more
    // like audio than the original 5-bar bargraph.
    constexpr int WAVE_H         = 30;
    constexpr int WAVE_Y         = RESP_Y + RESP_H - WAVE_H;  // bottom of response
    constexpr int WAVE_MID       = WAVE_Y + WAVE_H / 2;
    // Number of sample points across the screen. 64 = ~5 px between
    // points on the 320 px display — fluid enough that the line looks
    // continuous, sparse enough that fillRect cost stays trivial.
    constexpr int WAVE_POINTS    = 64;
    constexpr int WAVE_DX        = SCR_W / WAVE_POINTS;
    // Max excursion from the centerline. ±13 px keeps the waveform
    // visually inside the 30 px band with a 1 px margin top/bottom.
    constexpr int WAVE_AMPLITUDE = (WAVE_H / 2) - 2;
    // Refresh cadence — 10 Hz looks fluid without burning bus time.
    constexpr uint32_t WAVE_FRAME_MS = 100;
}

// ── Per-state colours and labels ─────────────────────────────────────────────
struct StateStyle {
    uint16_t bg;
    uint16_t fg;
    const char* label;
};

static constexpr StateStyle kStyles[] = {
    { TFT_BLACK,    TFT_DARKGREY, "IDLE"      },  // IDLE
    { TFT_BLUE,     TFT_WHITE,    "LISTENING" },  // LISTENING
    { TFT_YELLOW,   TFT_BLACK,    "THINKING"  },  // THINKING
    { TFT_GREEN,    TFT_BLACK,    "SPEAKING"  },  // SPEAKING
    { TFT_RED,      TFT_WHITE,    "ERROR"     },  // ERROR
};

// Cached state, so setStatus() (which fillRects the entire bar) can repaint
// the battery indicator with whatever values were last polled, and so
// updateBattery() (called outside a state change) knows which colours to
// use. -1 level means "not yet polled" — render an empty icon + "--%".
namespace {
    DeviceState g_current_state    = DeviceState::IDLE;
    int         g_battery_level    = -1;
    bool        g_battery_charging = false;

    // Waveform animation state. `g_wave_was_speaking` lets tickWaveform()
    // detect the SPEAKING→other transition and erase exactly once.
    bool      g_wave_was_speaking = false;
    uint32_t  g_wave_last_frame_ms = 0;
    // Tiny PRNG seed advanced per-bar per-frame. xorshift32 is fine for
    // visual jitter — we just want non-repeating bar heights.
    uint32_t  g_wave_seed         = 0x1234ABCD;

    // Paint the battery indicator into the status bar using the current
    // state's bg/fg. Called from setStatus() (after fillRect) and
    // updateBattery() (which fillRects just the battery region first).
    void drawBattery() {
        const StateStyle& s = kStyles[static_cast<int>(g_current_state)];
        const uint16_t bg = s.bg;
        const uint16_t fg = s.fg;

        // Outline + tip in fg.
        M5.Display.drawRect(BATT_BODY_X, BATT_BODY_Y,
                            BATT_BODY_W, BATT_BODY_H, fg);
        M5.Display.fillRect(BATT_TIP_X, BATT_TIP_Y,
                            BATT_TIP_W, BATT_TIP_H, fg);

        // Inside cleared to bg.
        M5.Display.fillRect(BATT_BODY_X + 1, BATT_BODY_Y + 1,
                            BATT_BODY_W - 2, BATT_BODY_H - 2, bg);

        // Fill bar: red below 20%, fg otherwise. Clamp 0..100.
        int pct = g_battery_level;
        if (pct >= 0) {
            if (pct > 100) pct = 100;
            int bar_w = ((BATT_BODY_W - 4) * pct) / 100;
            if (bar_w > 0) {
                uint16_t bar_color = (pct < 20) ? TFT_RED : fg;
                M5.Display.fillRect(BATT_BODY_X + 2, BATT_BODY_Y + 2,
                                    bar_w, BATT_BODY_H - 4, bar_color);
            }
        }

        // Charging glyph: small bolt overlaid in yellow. Drawn last so it
        // shows on top of any fill. Skipped on YELLOW backgrounds (THINKING
        // state) where it'd be invisible — fall back to a contrasting bolt.
        if (g_battery_charging) {
            uint16_t bolt = (s.bg == TFT_YELLOW) ? TFT_BLACK : TFT_YELLOW;
            int cx = BATT_BODY_X + BATT_BODY_W / 2;
            int top = BATT_BODY_Y + 2;
            int bot = BATT_BODY_Y + BATT_BODY_H - 3;
            int mid = BATT_BODY_Y + BATT_BODY_H / 2;
            // Lightning bolt: top-right → mid-left → bottom-right.
            M5.Display.drawLine(cx + 2, top, cx - 1, mid, bolt);
            M5.Display.drawLine(cx - 1, mid, cx + 2, mid, bolt);
            M5.Display.drawLine(cx + 2, mid, cx - 2, bot, bolt);
        }

        // Percentage text. Right-aligned at BATT_PCT_X+BATT_PCT_W.
        char buf[8];
        if (g_battery_level < 0) {
            snprintf(buf, sizeof(buf), " --%%");
        } else {
            snprintf(buf, sizeof(buf), "%3d%%", pct);
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(BATT_PCT_X, BATT_PCT_Y);
        M5.Display.print(buf);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void Display::begin() {
    M5.Display.setTextWrap(false);
    M5.Display.fillScreen(TFT_BLACK);

    M5.Display.drawFastHLine(0, DIV1_Y, SCR_W, TFT_DARKGREY);
    M5.Display.drawFastHLine(0, DIV2_Y, SCR_W, TFT_DARKGREY);

    setStatus(DeviceState::IDLE);
    updateFooter("OFF", 0);
}

void Display::setStatus(DeviceState state) {
    g_current_state = state;
    const StateStyle& s = kStyles[static_cast<int>(state)];

    M5.Display.fillRect(0, STATUS_Y, SCR_W, STATUS_H, s.bg);
    M5.Display.fillCircle(DOT_X, DOT_Y, DOT_R, s.fg);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(s.fg, s.bg);
    M5.Display.setCursor(LBL_X, LBL_Y);
    M5.Display.print(s.label);

    // fillRect wiped the battery region — repaint with cached values.
    drawBattery();
}

void Display::tickWaveform() {
    bool speaking = (g_current_state == DeviceState::SPEAKING);

    // Edge case: speaking → not speaking. Erase the strip back to black
    // exactly once, then leave it alone until the next SPEAKING entry.
    if (g_wave_was_speaking && !speaking) {
        M5.Display.fillRect(0, WAVE_Y, SCR_W, WAVE_H, TFT_BLACK);
        g_wave_was_speaking = false;
        return;
    }
    if (!speaking) return;

    // Throttle to ~10 Hz. millis() wraparound is harmless because we
    // compare with subtraction (uint32_t arithmetic).
    uint32_t now = millis();
    if (g_wave_was_speaking && (now - g_wave_last_frame_ms) < WAVE_FRAME_MS) {
        return;
    }
    g_wave_last_frame_ms = now;
    g_wave_was_speaking  = true;

    // xorshift32 — cheap and chaotic enough for waveform jitter.
    auto rand32 = [](uint32_t& s) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    };

    // Wipe the band, then draw connected line segments across the
    // width. Each point's Y is centerline ± a random offset; drawing
    // line(prev, curr) gives a continuous oscilloscope-style trace.
    M5.Display.fillRect(0, WAVE_Y, SCR_W, WAVE_H, TFT_BLACK);

    int prev_x = 0;
    uint32_t r0 = rand32(g_wave_seed);
    int prev_y = WAVE_MID + (int)((r0 % (uint32_t)(WAVE_AMPLITUDE * 2 + 1))) - WAVE_AMPLITUDE;

    for (int i = 1; i <= WAVE_POINTS; ++i) {
        int x = i * WAVE_DX;
        if (x >= SCR_W) x = SCR_W - 1;
        uint32_t r = rand32(g_wave_seed);
        int y = WAVE_MID
              + (int)((r % (uint32_t)(WAVE_AMPLITUDE * 2 + 1))) - WAVE_AMPLITUDE;
        M5.Display.drawLine(prev_x, prev_y, x, y, TFT_GREEN);
        prev_x = x;
        prev_y = y;
    }
}

void Display::updateBattery(int level, bool charging) {
    g_battery_level    = level;
    g_battery_charging = charging;

    // Wipe just the battery region (icon + pct text) with the current
    // state's bg, then redraw. Avoid touching the rest of the status bar.
    const StateStyle& s = kStyles[static_cast<int>(g_current_state)];
    int wipe_x = BATT_PCT_X;
    int wipe_w = (BATT_TIP_X + BATT_TIP_W) - BATT_PCT_X;
    M5.Display.fillRect(wipe_x, STATUS_Y, wipe_w, STATUS_H, s.bg);
    drawBattery();
}

void Display::showTranscript(const String& text) {
    M5.Display.fillRect(0, TRANS_Y, SCR_W, TRANS_H, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setTextWrap(true);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(4, TRANS_Y + 4);
    M5.Display.print(text);
    M5.Display.setTextWrap(false);
}

void Display::showResponse(const String& text) {
    M5.Display.fillRect(0, RESP_Y, SCR_W, RESP_H, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextWrap(true);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(4, RESP_Y + 4);
    M5.Display.print(text);
    M5.Display.setTextWrap(false);
}

void Display::updateFooter(const String& tier, int rssi) {
    M5.Display.fillRect(0, FOOT_Y, SCR_W, FOOT_H, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);

    // Left: tier + RSSI
    M5.Display.setCursor(4, FOOT_Y + (FOOT_H - 8) / 2);
    M5.Display.printf("%s  %ddBm", tier.c_str(), rssi);

    // Right: wall-clock (requires NTP; silently omitted until Phase 4+ wires it up)
    struct tm t;
    if (getLocalTime(&t, /*timeoutMs=*/0)) {
        char buf[9];
        strftime(buf, sizeof(buf), "%H:%M:%S", &t);
        // 8 chars × 6px/char (textSize=1) = 48px; right-align with 4px margin
        M5.Display.setCursor(SCR_W - 48 - 4, FOOT_Y + (FOOT_H - 8) / 2);
        M5.Display.print(buf);
    }
}

}  // namespace jarvis::hal

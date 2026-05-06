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
    const StateStyle& s = kStyles[static_cast<int>(state)];

    M5.Display.fillRect(0, STATUS_Y, SCR_W, STATUS_H, s.bg);
    M5.Display.fillCircle(DOT_X, DOT_Y, DOT_R, s.fg);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(s.fg, s.bg);
    M5.Display.setCursor(LBL_X, LBL_Y);
    M5.Display.print(s.label);
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

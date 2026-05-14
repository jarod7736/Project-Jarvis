#include "SettingsScreen.h"

#include <M5Unified.h>

#include "../hal/AudioPlayer.h"
#include "../hal/Display.h"
#include "../hal/LLMModule.h"
#include "NVSConfig.h"
#include "ModeManager.h"

namespace jarvis::app {

namespace {

// ── Palette (matches hal/Display.cpp's "phosphor" theme) ─────────────────
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t COL_BG       = rgb565(0x04, 0x06, 0x0A);
const uint16_t COL_BG2      = rgb565(0x08, 0x0D, 0x10);
const uint16_t COL_FG       = rgb565(0xFF, 0xD3, 0x88);
const uint16_t COL_DIM      = rgb565(0x7A, 0x5A, 0x2A);
const uint16_t COL_PRIMARY  = rgb565(0xFF, 0xAA, 0x2A);

// ── Layout ───────────────────────────────────────────────────────────────
constexpr int SCR_W = 320;
constexpr int SCR_H = 240;

// Close button — top-right. 48×28 hit zone, intentionally generous so the
// "X" is comfortable for a thumb. Renders as a small "[X]" glyph rather
// than a bare X so the tap target reads clearly against the chrome.
constexpr int CLOSE_W = 48;
constexpr int CLOSE_H = 28;
constexpr int CLOSE_X = SCR_W - CLOSE_W;
constexpr int CLOSE_Y = 0;

// Slider geometry. Each slider has a label row, a track, and a value
// readout. The hit zone for drags is the full row height (label+track+
// padding) so the user doesn't have to land precisely on the 14 px track.
constexpr int TRACK_X = 20;
constexpr int TRACK_W = SCR_W - 2 * TRACK_X;   // 280 px
constexpr int TRACK_H = 14;
constexpr int KNOB_R  = 9;

struct SliderRow {
    int  y_label;
    int  y_track;
    int  y_hit_top;   // generous touch capture region above and below the
    int  y_hit_bot;   // visible track, for forgiving thumb-on-glass hits.
};

constexpr SliderRow BRIGHTNESS_ROW = { 42,  64, 36,  88 };
constexpr SliderRow VOLUME_ROW     = {102, 124, 96, 148 };
constexpr SliderRow MICGAIN_ROW    = {162, 184, 156, 208 };

// Brightness: NVS range is [10, 255]. Below 10 the panel is unreadable;
// that's also the validation floor in NVSConfig::setBrightness.
constexpr int BRIGHT_MIN = 10;
constexpr int BRIGHT_MAX = 255;
// Volume / mic gain: 0..100 percent.
constexpr int PCT_MIN = 0;
constexpr int PCT_MAX = 100;

// ── State ────────────────────────────────────────────────────────────────
enum class DragTarget : uint8_t {
    None,
    Brightness,
    Volume,
    MicGain,
    Close,    // user tapped the close button — armed; finalized on release
};

DragTarget g_drag      = DragTarget::None;
int        g_brightness = 180;
int        g_volume     = 70;
int        g_mic_gain   = 50;

// Per-slider "needs save on release" flags. Set on any value change
// during drag; cleared after the NVS write fires on touch-up. Avoids
// burning a flash erase cycle when the user opens the screen and exits
// without actually moving anything.
bool g_brightness_dirty = false;
bool g_volume_dirty     = false;
bool g_micgain_dirty    = false;

// Non-owning. Set by SettingsScreen::setLLMModule() from main setup().
// Null until then — the slider tolerates that by skipping the hardware
// apply and only writing NVS.
jarvis::hal::LLMModule* g_llm_module = nullptr;

// ── Rendering ────────────────────────────────────────────────────────────

int valueToPos(int value, int vmin, int vmax) {
    if (vmax == vmin) return TRACK_X;
    int v = value;
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    int span = TRACK_W - 2 * KNOB_R;
    return TRACK_X + KNOB_R + (span * (v - vmin)) / (vmax - vmin);
}

int posToValue(int x, int vmin, int vmax) {
    int span = TRACK_W - 2 * KNOB_R;
    int rel  = x - (TRACK_X + KNOB_R);
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    return vmin + (rel * (vmax - vmin)) / span;
}

void drawSlider(const SliderRow& row, const char* label, int value, int vmin, int vmax) {
    // Wipe the row (label + track + padding) to background. Wiping the
    // whole hit zone keeps stale knob pixels from a previous value
    // hanging around.
    M5.Display.fillRect(0, row.y_hit_top, SCR_W, row.y_hit_bot - row.y_hit_top, COL_BG);

    // Label, left-aligned.
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_FG, COL_BG);
    M5.Display.setCursor(TRACK_X, row.y_label);
    M5.Display.print(label);

    // Value readout, right-aligned. Three-digit field so the digits
    // don't jitter as the value crosses 10 / 100.
    char buf[8];
    snprintf(buf, sizeof(buf), "%3d", value);
    M5.Display.setTextColor(COL_PRIMARY, COL_BG);
    M5.Display.setCursor(SCR_W - TRACK_X - 18, row.y_label);
    M5.Display.print(buf);

    // Track background (dim) + frame.
    M5.Display.fillRoundRect(TRACK_X, row.y_track, TRACK_W, TRACK_H, 4, COL_BG2);
    M5.Display.drawRoundRect(TRACK_X, row.y_track, TRACK_W, TRACK_H, 4, COL_DIM);

    // Filled portion — from track left to current knob center.
    int knob_x = valueToPos(value, vmin, vmax);
    int fill_w = knob_x - TRACK_X;
    if (fill_w > 2) {
        M5.Display.fillRoundRect(TRACK_X + 1, row.y_track + 1,
                                 fill_w - 1, TRACK_H - 2, 3, COL_PRIMARY);
    }

    // Knob — circle on top of the track, centered vertically.
    int knob_y = row.y_track + TRACK_H / 2;
    M5.Display.fillCircle(knob_x, knob_y, KNOB_R, COL_FG);
    M5.Display.drawCircle(knob_x, knob_y, KNOB_R, COL_PRIMARY);
}

void drawCloseButton() {
    M5.Display.fillRect(CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H, COL_BG);
    M5.Display.drawRoundRect(CLOSE_X + 6, CLOSE_Y + 4, CLOSE_W - 12, CLOSE_H - 8,
                             4, COL_DIM);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_PRIMARY, COL_BG);
    // Glyph is the multiplication sign for a clean X without ascii's
    // "wider top, narrow bottom" look. Two-char string for centering.
    M5.Display.setCursor(CLOSE_X + (CLOSE_W - 12) / 2, CLOSE_Y + 7);
    M5.Display.print("X");
}

void drawChrome() {
    // Title left of the close button.
    M5.Display.fillRect(0, 0, CLOSE_X, CLOSE_H, COL_BG);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_DIM, COL_BG);
    M5.Display.setCursor(8, 4);
    M5.Display.print("SETTINGS");
    M5.Display.setTextColor(COL_FG, COL_BG);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 12);
    M5.Display.print("ADJUST");

    // Thin separator below the title bar.
    M5.Display.drawFastHLine(0, CLOSE_H, SCR_W, COL_DIM);
}

void drawAll() {
    M5.Display.fillScreen(COL_BG);
    drawChrome();
    drawCloseButton();
    drawSlider(BRIGHTNESS_ROW, "BRIGHTNESS", g_brightness, BRIGHT_MIN, BRIGHT_MAX);
    drawSlider(VOLUME_ROW,     "VOLUME",     g_volume,     PCT_MIN,    PCT_MAX);
    drawSlider(MICGAIN_ROW,    "MIC GAIN",   g_mic_gain,   PCT_MIN,    PCT_MAX);

    // Footer hint — small, dim, reminds the user how to leave.
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_DIM, COL_BG);
    M5.Display.setCursor(20, 226);
    M5.Display.print("Tap [X] to close");
}

// ── Touch handling ───────────────────────────────────────────────────────

bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

bool inSliderHit(int y, const SliderRow& row) {
    return y >= row.y_hit_top && y < row.y_hit_bot;
}

void applyAndRedraw(DragTarget target, int new_value) {
    switch (target) {
        case DragTarget::Brightness:
            if (new_value == g_brightness) return;
            g_brightness = new_value;
            g_brightness_dirty = true;
            // Live apply — PWM duty write, atomic from any context.
            jarvis::hal::Display::setBrightness(g_brightness);
            drawSlider(BRIGHTNESS_ROW, "BRIGHTNESS", g_brightness,
                       BRIGHT_MIN, BRIGHT_MAX);
            break;
        case DragTarget::Volume:
            if (new_value == g_volume) return;
            g_volume = new_value;
            g_volume_dirty = true;
            // Live apply — speaker mixer scale, per-sample.
            jarvis::hal::AudioPlayer::setVolume(g_volume);
            drawSlider(VOLUME_ROW, "VOLUME", g_volume, PCT_MIN, PCT_MAX);
            break;
        case DragTarget::MicGain:
            if (new_value == g_mic_gain) return;
            g_mic_gain = new_value;
            g_micgain_dirty = true;
            // No per-tick apply — the StackFlow audio chain has to be
            // torn down + re-setup to take a new capVolume (there's no
            // `update` action), and doing that on every drag delta would
            // shred the mic. Apply happens once on touch-up in
            // persistDirty().
            drawSlider(MICGAIN_ROW, "MIC GAIN", g_mic_gain, PCT_MIN, PCT_MAX);
            break;
        default:
            break;
    }
}

void persistDirty() {
    if (g_brightness_dirty) {
        jarvis::NVSConfig::setBrightness(g_brightness);
        g_brightness_dirty = false;
    }
    if (g_volume_dirty) {
        jarvis::NVSConfig::setTtsVolume(g_volume);
        g_volume_dirty = false;
    }
    if (g_micgain_dirty) {
        // NVS first so the value is durable even if the soft-restart
        // hangs or wedges the pipeline — a reboot will then pick up the
        // intended gain at boot-time setup.
        jarvis::NVSConfig::setMicGain(g_mic_gain);
        g_micgain_dirty = false;

        // Apply to the live pipeline if we have a module pointer. This
        // is the audio.exit → audio.setup → audio.work → kws.setup →
        // asr.setup soft-restart; runs synchronously and takes a few
        // seconds. We do NOT gate on FSM state — the voice pipeline is
        // already paused while SettingsScreen is up (main.cpp returns
        // early on isSettings()), so there's no in-flight query to
        // interrupt. applyMicGain() logs each step; on failure the
        // device may need a manual reboot to recover.
        if (g_llm_module != nullptr) {
            (void)g_llm_module->applyMicGain(g_mic_gain);
        }
    }
}

}  // namespace

void SettingsScreen::setLLMModule(jarvis::hal::LLMModule* module) {
    g_llm_module = module;
}

void SettingsScreen::enter() {
    // Seed local state from NVS so the rendered position reflects what's
    // actually stored. Defaults match NVSConfig's fallbacks (180 / 70 / 50).
    g_brightness = jarvis::NVSConfig::getBrightness();
    g_volume     = jarvis::NVSConfig::getTtsVolume();
    g_mic_gain   = jarvis::NVSConfig::getMicGain();

    g_drag             = DragTarget::None;
    g_brightness_dirty = false;
    g_volume_dirty     = false;
    g_micgain_dirty    = false;

    drawAll();
}

void SettingsScreen::exit() {
    // Defensive flush — in normal use persistDirty() already fired on
    // touch-up, but if the user closes the screen mid-drag (e.g. lifts
    // and immediately taps Close in the same frame) we don't want to
    // drop a pending value.
    persistDirty();
    g_drag = DragTarget::None;
}

void SettingsScreen::tick() {
    if (M5.Touch.getCount() == 0) {
        if (g_drag != DragTarget::None) {
            // Touch-up: persist any pending changes; clear drag state.
            // The Close target intentionally does NOT fire here — it
            // fires below on a release-inside-the-button.
            persistDirty();
            g_drag = DragTarget::None;
        }
        return;
    }

    const auto& td = M5.Touch.getDetail();

    if (td.wasPressed()) {
        // Hit-test against close button first, then sliders.
        if (inRect(td.x, td.y, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H)) {
            g_drag = DragTarget::Close;
            return;
        }
        if (inSliderHit(td.y, BRIGHTNESS_ROW)) {
            g_drag = DragTarget::Brightness;
            applyAndRedraw(g_drag, posToValue(td.x, BRIGHT_MIN, BRIGHT_MAX));
            return;
        }
        if (inSliderHit(td.y, VOLUME_ROW)) {
            g_drag = DragTarget::Volume;
            applyAndRedraw(g_drag, posToValue(td.x, PCT_MIN, PCT_MAX));
            return;
        }
        if (inSliderHit(td.y, MICGAIN_ROW)) {
            g_drag = DragTarget::MicGain;
            applyAndRedraw(g_drag, posToValue(td.x, PCT_MIN, PCT_MAX));
            return;
        }
        g_drag = DragTarget::None;
        return;
    }

    if (td.wasReleased()) {
        if (g_drag == DragTarget::Close &&
            inRect(td.x, td.y, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H)) {
            // Tap on close: confirmed (down-and-up inside the button).
            // Drag off the button before releasing → cancelled.
            ModeManager::exitSettings();
            return;
        }
        persistDirty();
        g_drag = DragTarget::None;
        return;
    }

    if (td.isPressed()) {
        switch (g_drag) {
            case DragTarget::Brightness:
                applyAndRedraw(g_drag, posToValue(td.x, BRIGHT_MIN, BRIGHT_MAX));
                break;
            case DragTarget::Volume:
                applyAndRedraw(g_drag, posToValue(td.x, PCT_MIN, PCT_MAX));
                break;
            case DragTarget::MicGain:
                applyAndRedraw(g_drag, posToValue(td.x, PCT_MIN, PCT_MAX));
                break;
            case DragTarget::Close:
            case DragTarget::None:
                break;
        }
    }
}

}  // namespace jarvis::app

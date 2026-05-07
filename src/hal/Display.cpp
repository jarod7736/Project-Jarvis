// Display.cpp -- Jarvis on-device UI for CoreS3.
//
// Faithful port of the p5.js particle-field prototype
// (docs/jarvis_particle_prototype.html). The 320×240 screen is owned
// by a particle simulation that encodes:
//   - FSM state via motion pattern  (idle / capturing / thinking /
//     speaking / error)
//   - Routing tier via particle color (qwen / local / cloud / ha)
//
// Implementation notes:
//   - All rendering goes through an M5Canvas sprite allocated in
//     PSRAM (320·240·2B = 153 KB). pushSprite() is a single DMA blit,
//     so we get flicker-free 30 FPS without burning bus time on
//     dozens of fillRect/drawLine calls per frame.
//   - The public methods setStatus() / updateBattery() / showTranscript()
//     etc. only update internal state; they don't paint directly.
//     tickParticles() reads that state and produces the next frame.
//   - drawConfigScreen() bypasses the particle system entirely (Config
//     mode is a static screen managed by ModeManager — no animation
//     needed and we want it readable while voice is paused).
//
// Performance notes:
//   - 70 particles × 30 FPS × ~20 float ops per particle update
//     ≈ 42K float ops/sec. ESP32-S3 FPU eats this for breakfast.
//   - Per-frame collision pass for the IDLE 'disperse' behavior is
//     O(n²) over particles. With n ≤ 64 (kMaxParticles) that's <2K
//     pair tests per frame, also trivial.
//   - sinf() in newlib is software but fast on ESP32-S3 (~1 µs).

#include "Display.h"

#include <M5Unified.h>
#include <math.h>
#include <time.h>

namespace jarvis::hal {

// ── Layout ──────────────────────────────────────────────────────────────────
namespace {
constexpr int SCR_W   = 320;
constexpr int SCR_H   = 240;
constexpr int STATUS_H = 24;        // top status bar
constexpr int kMaxParticles = 64;   // ≤ 70 in the prototype, dialed down
                                    // for headroom on ESP32-S3
constexpr uint32_t kFrameMs = 33;   // ~30 FPS

// ── Particle behavior table ─────────────────────────────────────────────────
enum class TargetType : uint8_t { Disperse, Horizontal, Orbit, Wave, Gravity };

struct Behavior {
    int   count;
    float drift;
    float damping;
    float target_force;
    TargetType target_type;
    // Type-specific extras (zero if unused for that target)
    float repel_radius, repel_strength;        // Disperse
    float oscillate_amp, oscillate_freq;       // Horizontal
    float orbit_radius, orbit_speed;           // Orbit
    float wave_amp, wave_freq, wave_speed;     // Wave
    float gravity;                             // Gravity
    // Sizing & alpha
    float size_min, size_max;
    int   alpha_base, alpha_pulse;
    uint32_t pulse_period_ms;
    float desaturate;  // 0..1; ERROR mutes the tier color
};

// Indexed by DeviceState (IDLE=0, LISTENING=1, THINKING=2, SPEAKING=3, ERROR=4)
//
// Sizes are deliberately larger than the JS prototype's; on a 320×240
// integer-pixel raster a "size 1.2" particle's 0.6-pixel core radius
// rounds to 1 px for everyone, killing the size variation. ~2-6 px
// puts each layer of the glow at a distinct integer radius and keeps
// the per-particle size_seed visibly different.
constexpr Behavior kBehaviors[] = {
    // IDLE — gentle dispersion, particles repel each other softly
    { 42, 0.18f, 0.94f, 0.0008f, TargetType::Disperse,
      38.0f, 0.012f,
      0,0, 0,0, 0,0,0, 0,
      2.0f, 4.5f, 90, 30, 4000, 0.0f },
    // LISTENING (capturing) — horizontal oscillation pumped by mic level
    { 56, 0.6f, 0.88f, 0.012f, TargetType::Horizontal,
      0,0,
      36.0f, 0.012f,
      0,0, 0,0,0, 0,
      2.5f, 5.5f, 140, 40, 600, 0.0f },
    // THINKING — orbital motion around screen center
    { 42, 0.1f, 0.92f, 0.018f, TargetType::Orbit,
      0,0, 0,0,
      38.0f, 0.018f,
      0,0,0, 0,
      2.2f, 5.0f, 130, 70, 1100, 0.0f },
    // SPEAKING — sine wave ripple across the field
    { 60, 0.4f, 0.86f, 0.008f, TargetType::Wave,
      0,0, 0,0, 0,0,
      22.0f, 0.025f, 0.06f,
      0,
      3.0f, 6.0f, 160, 50, 350, 0.0f },
    // ERROR — sparse, falling, color desaturated
    { 16, 0.04f, 0.98f, 0.0006f, TargetType::Gravity,
      0,0, 0,0, 0,0, 0,0,0,
      0.02f,
      1.5f, 3.0f, 50, 10, 5000, 0.7f },
};

// ── Tier table ──────────────────────────────────────────────────────────────
struct TierInfo {
    const char* name;
    uint8_t r, g, b;
};
constexpr TierInfo kTiers[] = {
    { "QWEN",  0x5F, 0xE3, 0xA1 },  // mint   (Qwen — on-Module router/local)
    { "LOCAL", 0x7F, 0xC8, 0xFF },  // blue   (OpenClaw gemma)
    { "CLOUD", 0xC8, 0x9C, 0xFF },  // purple (Claude via OpenClaw)
    { "HA",    0xFF, 0xB4, 0x54 },  // orange (Home Assistant)
};

// ── Particle ────────────────────────────────────────────────────────────────
struct Particle {
    float x, y;
    float vx, vy;
    float size_seed;   // 0..1 — sized within behavior min..max
    float phase;       // alpha-pulse phase offset
    float seed;        // orbit-radius variation, etc.
};

Particle g_particles[kMaxParticles];
int      g_active = 42;

// State + cache
DeviceState g_state          = DeviceState::IDLE;
DeviceState g_state_prev     = DeviceState::IDLE;
int         g_tier_idx       = 0;     // QWEN by default until main wires getConnectivityTier()
uint32_t    g_last_frame_ms  = 0;
uint32_t    g_seed           = 0x1234ABCD;
float       g_mic_level      = 0.0f;  // 0..1 — synthesized pseudo-amplitude

int    g_battery_level    = -1;
bool   g_battery_charging = false;
String g_wifi_tier        = "...";
int    g_wifi_rssi        = 0;
bool   g_ota_active       = false;

// Overlay card state — populated by showTranscript/showResponse, fades
// out after kOverlayHoldMs of inactivity.
constexpr uint32_t kOverlayHoldMs = 6000;
String   g_overlay_label;
String   g_overlay_line;
String   g_overlay_meta;
uint32_t g_overlay_until = 0;

// Sprite — allocated lazily on first begin(). We use M5Canvas
// (LovyanGFX's sprite) at 16-bpp; createSprite() prefers PSRAM when
// available (the CoreS3 has 8 MB).
M5Canvas g_canvas(&M5.Display);
bool     g_canvas_ready = false;

// ── PRNG / math helpers ─────────────────────────────────────────────────────
inline uint32_t xs32() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
inline float frand() {
    // 24-bit fraction, plenty of resolution for visual jitter.
    return (float)(xs32() & 0xFFFFFF) / (float)0xFFFFFF;
}
inline float frand_signed() { return frand() * 2.0f - 1.0f; }

// ── Color helpers ───────────────────────────────────────────────────────────
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
struct ColorRGB { uint8_t r, g, b; };
ColorRGB tierColor(int idx, float desat) {
    const TierInfo& t = kTiers[idx];
    if (desat <= 0.0f) return { t.r, t.g, t.b };
    float lum = 0.299f * t.r + 0.587f * t.g + 0.114f * t.b;
    return {
        (uint8_t)(t.r + (lum - t.r) * desat + 0.5f),
        (uint8_t)(t.g + (lum - t.g) * desat + 0.5f),
        (uint8_t)(t.b + (lum - t.b) * desat + 0.5f),
    };
}
// argb8888 with real alpha — the canvas is 32bpp (RGBA8888) so
// LovyanGFX does proper per-pixel alpha blending against whatever's
// already in the sprite. M5Canvas::pushSprite() down-converts to
// RGB565 on the way to the panel.
inline uint32_t argb(const ColorRGB& c, int alpha) {
    if (alpha < 0)   alpha = 0;
    if (alpha > 255) alpha = 255;
    return ((uint32_t)alpha << 24) |
           ((uint32_t)c.r   << 16) |
           ((uint32_t)c.g   <<  8) |
           (uint32_t)c.b;
}

// ── Particle sim ────────────────────────────────────────────────────────────
void rebuildParticles(int target_count) {
    if (target_count > kMaxParticles) target_count = kMaxParticles;
    // Spawn newcomers across the particle field area only (top STATUS_H
    // rows are reserved for the bar).
    while (g_active < target_count) {
        Particle& p = g_particles[g_active];
        p.x         = frand() * SCR_W;
        p.y         = frand() * (SCR_H - STATUS_H);
        p.vx        = frand_signed() * 0.5f;
        p.vy        = frand_signed() * 0.5f;
        p.size_seed = frand();
        p.phase     = frand() * 6.283185f;
        p.seed      = frand();
        ++g_active;
    }
    if (g_active > target_count) g_active = target_count;
}

void updateParticles(const Behavior& b, float t) {
    const float fw = (float)SCR_W;
    const float fh = (float)(SCR_H - STATUS_H);

    // Disperse: O(n²) repel pass. Skipped if not the active target.
    if (b.target_type == TargetType::Disperse) {
        const float r2 = b.repel_radius * b.repel_radius;
        for (int i = 0; i < g_active; ++i) {
            for (int j = i + 1; j < g_active; ++j) {
                float dx = g_particles[i].x - g_particles[j].x;
                float dy = g_particles[i].y - g_particles[j].y;
                float d2 = dx * dx + dy * dy;
                if (d2 > 1.0f && d2 < r2) {
                    float d = sqrtf(d2);
                    float f = b.repel_strength * (1.0f - d2 / r2);
                    float fx = (dx / d) * f;
                    float fy = (dy / d) * f;
                    g_particles[i].vx += fx; g_particles[i].vy += fy;
                    g_particles[j].vx -= fx; g_particles[j].vy -= fy;
                }
            }
        }
    }

    for (int i = 0; i < g_active; ++i) {
        Particle& p = g_particles[i];
        p.vx += frand_signed() * b.drift;
        p.vy += frand_signed() * b.drift;

        switch (b.target_type) {
            case TargetType::Horizontal: {
                float ty = fh * 0.5f
                         + sinf(p.x * b.oscillate_freq + t * 0.05f)
                           * b.oscillate_amp * (0.4f + g_mic_level);
                p.vy += (ty - p.y) * b.target_force;
                p.vx += (fw * 0.5f - p.x) * b.target_force * 0.3f;
                break;
            }
            case TargetType::Orbit: {
                float cx = fw * 0.5f, cy = fh * 0.5f;
                float dx = p.x - cx, dy = p.y - cy;
                float dist = sqrtf(dx * dx + dy * dy) + 0.01f;
                float td   = b.orbit_radius * (0.7f + 0.6f * p.seed);
                p.vx += (-dy / dist) * b.orbit_speed;
                p.vy += ( dx / dist) * b.orbit_speed;
                float radial_err = (td - dist) * b.target_force;
                p.vx += (dx / dist) * radial_err;
                p.vy += (dy / dist) * radial_err;
                break;
            }
            case TargetType::Wave: {
                float ty = fh * 0.5f
                         + sinf(p.x * b.wave_freq + t * b.wave_speed)
                           * b.wave_amp * (0.5f + g_mic_level);
                p.vy += (ty - p.y) * b.target_force;
                break;
            }
            case TargetType::Gravity:
                p.vy += b.gravity;
                break;
            case TargetType::Disperse:
            default:
                // Forces already applied above for Disperse.
                break;
        }

        p.vx *= b.damping;
        p.vy *= b.damping;
        p.x  += p.vx;
        p.y  += p.vy;

        // Wrap horizontally; bounce vertically inside the field.
        if (p.x < 0)   p.x += fw;
        if (p.x > fw)  p.x -= fw;
        if (p.y < 4)             { p.y = 4;        p.vy = fabsf(p.vy) * 0.5f; }
        if (p.y > fh - 4)        { p.y = fh - 4;   p.vy = -fabsf(p.vy) * 0.5f; }
    }
}

void drawParticles(const Behavior& b, const ColorRGB& base, float t) {
    // Field is offset by STATUS_H from top.
    for (int i = 0; i < g_active; ++i) {
        Particle& p = g_particles[i];
        float sz = b.size_min + (b.size_max - b.size_min) * p.size_seed;
        float phase = (t * 1000.0f / (float)b.pulse_period_ms) * 6.283185f + p.phase;
        float alpha = (float)b.alpha_base + sinf(phase) * (float)b.alpha_pulse;
        if (alpha < 0)   alpha = 0;
        if (alpha > 255) alpha = 255;

        int cx = (int)p.x;
        int cy = (int)p.y + STATUS_H;
        // Five-layer "glow": each ring is a decreasing-radius
        // fillCircle with a real alpha value (canvas is 32bpp). The
        // outer rings are very dim and largely additive; the inner
        // rings stack alpha on top to produce a smooth-ish radial
        // falloff. With 32bpp blending this reads as a soft cloud
        // rather than the concentric solid rings the 16bpp code drew.
        int r5 = (int)(sz * 4.0f + 0.5f);
        int r4 = (int)(sz * 2.8f + 0.5f);
        int r3 = (int)(sz * 1.9f + 0.5f);
        int r2 = (int)(sz * 1.2f + 0.5f);
        int r1 = (int)(sz * 0.6f + 0.5f);
        if (r5 < 2) r5 = 2;
        if (r4 < 2) r4 = 2;
        if (r3 < 1) r3 = 1;
        if (r2 < 1) r2 = 1;
        if (r1 < 1) r1 = 1;

        g_canvas.fillCircle(cx, cy, r5, argb(base, (int)(alpha * 0.06f)));
        g_canvas.fillCircle(cx, cy, r4, argb(base, (int)(alpha * 0.13f)));
        g_canvas.fillCircle(cx, cy, r3, argb(base, (int)(alpha * 0.27f)));
        g_canvas.fillCircle(cx, cy, r2, argb(base, (int)(alpha * 0.55f)));
        g_canvas.fillCircle(cx, cy, r1, argb(base, (int)alpha));
    }
}

// ── Status bar ──────────────────────────────────────────────────────────────
void drawStatusBar(const ColorRGB& base) {
    // Bar background: very dark teal, semi over the field.
    g_canvas.fillRect(0, 0, SCR_W, STATUS_H, rgb565(13, 22, 20));
    g_canvas.drawFastHLine(0, STATUS_H, SCR_W, rgb565(31, 42, 48));

    uint16_t accent = rgb565(base.r, base.g, base.b);

    // Battery icon (left). 18×10 body + 2×4 tip.
    g_canvas.drawRect(8, 6, 18, 10, accent);
    g_canvas.fillRect(26, 9, 2, 4, accent);
    if (g_battery_level > 0) {
        int pct = g_battery_level > 100 ? 100 : g_battery_level;
        int bw = (14 * pct) / 100;
        if (bw > 0) g_canvas.fillRect(10, 8, bw, 6, accent);
    }
    g_canvas.setTextSize(1);
    g_canvas.setTextDatum(textdatum_t::middle_left);
    g_canvas.setTextColor(accent);
    char buf[8];
    if (g_battery_level < 0) snprintf(buf, sizeof(buf), "--%%");
    else                     snprintf(buf, sizeof(buf), "%d%%", g_battery_level);
    g_canvas.drawString(buf, 32, 12);
    if (g_battery_charging) {
        g_canvas.setTextColor(rgb565(0xFF, 0xB4, 0x54));
        g_canvas.drawString("+", 55, 12);
    }

    // OTA badge (only while active) — yellow, sits between battery and wifi.
    if (g_ota_active) {
        g_canvas.setTextColor(rgb565(0xFF, 0xB4, 0x54));
        g_canvas.drawString("OTA", 70, 12);
    }

    // WiFi connection arc (center-right)
    int wx = 200, wy = 14;
    bool offline = g_wifi_tier.equalsIgnoreCase("OFF") || g_wifi_tier.length() == 0
                   || g_wifi_tier.equals("...");
    uint16_t wifi_col = offline ? rgb565(0x80, 0x40, 0x40) : accent;
    // Two concentric arcs + a center dot. drawArc takes start/end in degrees.
    g_canvas.drawArc(wx, wy, 7, 6, 200, 340, wifi_col);
    g_canvas.drawArc(wx, wy, 4, 3, 200, 340, wifi_col);
    g_canvas.fillCircle(wx, wy - 1, 1, wifi_col);

    // Tier badge (right) — outlined pill with the tier name.
    const char* badge = kTiers[g_tier_idx].name;
    g_canvas.setTextSize(1);
    g_canvas.setTextDatum(textdatum_t::middle_center);
    int badge_w = strlen(badge) * 6 + 16;
    int badge_x = SCR_W - badge_w - 6;
    g_canvas.fillRect(badge_x, 4, badge_w, 16,
                      rgb565(base.r / 5, base.g / 5, base.b / 5));
    g_canvas.drawRect(badge_x, 4, badge_w, 16, accent);
    g_canvas.setTextColor(accent);
    g_canvas.drawString(badge, badge_x + badge_w / 2, 12);
    g_canvas.setTextDatum(textdatum_t::top_left);
}

// ── Overlay card ────────────────────────────────────────────────────────────
void drawOverlayCard() {
    if (millis() > g_overlay_until) return;
    if (g_overlay_line.length() == 0) return;

    constexpr int kCardH = 56;
    int cardY = SCR_H - kCardH - 8;
    g_canvas.fillRect(8, cardY, SCR_W - 16, kCardH, rgb565(6, 12, 11));
    g_canvas.drawRect(8, cardY, SCR_W - 16, kCardH, rgb565(31, 42, 48));

    g_canvas.setTextSize(1);
    g_canvas.setTextColor(rgb565(90, 138, 122));
    g_canvas.setTextDatum(textdatum_t::top_left);
    g_canvas.drawString(g_overlay_label.c_str(), 16, cardY + 6);

    g_canvas.setTextColor(rgb565(216, 228, 226));
    g_canvas.drawString(g_overlay_line.c_str(), 16, cardY + 22);

    if (g_overlay_meta.length() > 0) {
        g_canvas.setTextColor(rgb565(90, 138, 122));
        g_canvas.setTextDatum(textdatum_t::top_right);
        g_canvas.drawString(g_overlay_meta.c_str(), SCR_W - 16, cardY + 6);
        g_canvas.setTextDatum(textdatum_t::top_left);
    }
}

// State-to-mic-level synth. The mic-level field pumps the horizontal
// oscillation in LISTENING and the wave amplitude in SPEAKING. We
// don't have a real audio level here yet, so we synthesize one with
// nested sin() — close enough to "voice in progress" for visual.
void synthMicLevel(float t) {
    if (g_state == DeviceState::LISTENING) {
        g_mic_level = 0.3f + 0.6f * (0.5f + 0.5f * sinf(t * 4.0f + sinf(t * 11.0f) * 2.0f));
    } else if (g_state == DeviceState::SPEAKING) {
        g_mic_level = 0.4f + 0.5f * (0.5f + 0.5f * sinf(t * 7.0f + sinf(t * 13.0f) * 1.5f));
    } else {
        g_mic_level = 0.0f;
    }
}
}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────
void Display::begin() {
    M5.Display.setTextWrap(false);
    M5.Display.fillScreen(TFT_BLACK);

    // Allocate the full-screen sprite at 32bpp (RGBA8888) — the
    // particle "glow" effect needs real per-pixel alpha blending,
    // which 16bpp RGB565 doesn't provide. 320×240×4 = 307 KB, lives
    // in PSRAM (CoreS3 has 8 MB; LGFX prefers PSRAM when available).
    // pushSprite() down-converts to RGB565 on the way to the panel.
    g_canvas.setColorDepth(32);
    if (g_canvas.createSprite(SCR_W, SCR_H)) {
        g_canvas_ready = true;
        g_canvas.fillScreen(TFT_BLACK);
        Serial.printf("[Display] sprite OK 32bpp %dx%d (%u bytes)\n",
                      SCR_W, SCR_H, (unsigned)(SCR_W * SCR_H * 4));
    } else {
        // PSRAM-allocation failure path. Retry at 16bpp so we at least
        // get something on screen, even though the glow won't blend
        // properly without a real alpha channel.
        Serial.println("[Display] 32bpp sprite alloc FAILED — retrying at 16bpp");
        g_canvas.setColorDepth(16);
        g_canvas_ready = g_canvas.createSprite(SCR_W, SCR_H);
        if (g_canvas_ready) g_canvas.fillScreen(TFT_BLACK);
        else Serial.println("[Display] 16bpp sprite ALSO failed — direct draw fallback");
    }

    rebuildParticles(kBehaviors[(int)DeviceState::IDLE].count);
}

void Display::setStatus(DeviceState state) {
    if (state != g_state) {
        g_state_prev = g_state;
        g_state      = state;
        rebuildParticles(kBehaviors[(int)state].count);
    }
}

void Display::setTier(Tier t) {
    int idx = (int)t;
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    g_tier_idx = idx;
}

void Display::tickParticles() {
    if (!g_canvas_ready) return;

    uint32_t now = millis();
    if (now - g_last_frame_ms < kFrameMs) return;
    g_last_frame_ms = now;

    float t = now * 0.001f;
    synthMicLevel(t);

    const Behavior& b = kBehaviors[(int)g_state];
    ColorRGB base = tierColor(g_tier_idx, b.desaturate);

    // Background — slightly different on ERROR (warm tint).
    if (g_state == DeviceState::ERROR) {
        g_canvas.fillScreen(rgb565(8, 6, 6));
    } else {
        g_canvas.fillScreen(rgb565(6, 8, 8));
    }

    updateParticles(b, t);
    drawParticles(b, base, t);

    drawStatusBar(base);
    drawOverlayCard();

    g_canvas.pushSprite(0, 0);
}

void Display::updateBattery(int level, bool charging) {
    g_battery_level    = level;
    g_battery_charging = charging;
    // Reflected on the next tickParticles() — no immediate paint.
}

void Display::showTranscript(const String& text) {
    // Maps to the "HEARING" / "ROUTING" / "LAST" labels from the
    // prototype depending on what state we're currently in.
    const char* label;
    switch (g_state) {
        case DeviceState::LISTENING: label = "HEARING";  break;
        case DeviceState::THINKING:  label = "ROUTING";  break;
        default:                     label = "LAST";     break;
    }
    g_overlay_label = label;
    g_overlay_line  = String("\"") + text + "\"";
    g_overlay_meta  = "";
    g_overlay_until = millis() + kOverlayHoldMs;
}

void Display::showResponse(const String& text) {
    g_overlay_label = "REPLY";
    g_overlay_line  = text;
    g_overlay_meta  = kTiers[g_tier_idx].name;
    g_overlay_until = millis() + kOverlayHoldMs;
}

void Display::updateFooter(const String& tier, int rssi) {
    // The prototype doesn't have a footer per se — we fold the WiFi
    // connection state and tier into the top status bar. Keep the
    // method to preserve the existing FSM/main.cpp wiring.
    g_wifi_tier = tier;
    g_wifi_rssi = rssi;
}

void Display::setOtaActive(bool active) {
    g_ota_active = active;
}

void Display::setBrightness(int v) {
    if (v < 10)  v = 10;
    if (v > 255) v = 255;
    M5.Display.setBrightness(static_cast<uint8_t>(v));
}

void Display::drawConfigScreen() {
    // Bypass the particle system. Static screen, painted once on
    // entry to Config mode. Voice pipeline ticks (incl. tickParticles)
    // are gated off in main.cpp while Config is active.
    M5.Display.fillScreen(0x0841);
    M5.Display.setTextDatum(top_left);

    M5.Display.setTextColor(0x07E0);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(60, 20);
    M5.Display.print("CONFIG MODE");

    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(20, 70);
    M5.Display.print("Connect phone to WiFi:");

    M5.Display.setTextSize(2);
    M5.Display.setCursor(40, 100);
    M5.Display.setTextColor(0x07E0);
    M5.Display.print("Jarvis-Setup");

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(20, 150);
    M5.Display.print("Then open in browser:");
    M5.Display.setCursor(40, 170);
    M5.Display.setTextColor(0x07E0);
    M5.Display.print("http://192.168.4.1");

    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.setCursor(40, 220);
    M5.Display.print("Hold screen 2s to exit");
}

}  // namespace jarvis::hal

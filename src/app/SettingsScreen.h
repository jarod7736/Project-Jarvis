#pragma once

// SettingsScreen — on-device slider UI for the three runtime tunables
// the user is most likely to want to adjust without opening the captive
// portal: display brightness, TTS speaker volume, and mic input gain.

namespace jarvis::hal { class LLMModule; }
//
// Entered via a swipe-down-from-top-left gesture (detected in
// ModeManager). While up, the voice pipeline is paused (main.cpp gates
// it on !ModeManager::isSettings()) and this screen owns every pixel of
// the 320×240 panel — drag a slider to change a value; tap the close
// button (top-right "X") to exit.
//
// Live-apply pattern, parity with the captive portal's hook in
// ConfigSchema::liveApply:
//   - Brightness → Display::setBrightness() on every drag delta. PWM
//     write; cheap. NVS write only on touch-up so we don't thrash the
//     flash during a drag.
//   - Volume     → AudioPlayer::setVolume() on every drag delta.
//                  Per-sample mixer scale; cheap. NVS write on touch-up.
//   - Mic gain   → stored only (NVS write on touch-up). The
//                  LLMModule audio.work parameter is not yet wired
//                  through, per the TODO at app/ConfigSchema.cpp:176;
//                  the slider currently takes effect at the next boot.

namespace jarvis::app {

class SettingsScreen {
public:
    // Wire the LLMModule that the mic-gain slider should soft-restart on
    // touch-up. Call once from main setup() after LLMModule::begin()
    // succeeds. Passing nullptr (or never calling this at all) makes
    // mic-gain stored-only — the slider still writes NVS, but no audio
    // pipeline restart is attempted. SettingsScreen treats the pointer
    // as non-owning and never deletes it.
    static void setLLMModule(jarvis::hal::LLMModule* module);

    // Paint the screen once, seed slider values from NVS. Called by
    // ModeManager::enterSettings() via redrawAfterModeChange().
    static void enter();

    // Tear down any in-flight drag state. Called by
    // ModeManager::exitSettings() before the home screen is repainted.
    // The NVS values stay as they were — exit just lets go.
    static void exit();

    // Service one frame of touch input. Called from
    // ModeManager::tick() while the screen is up. Cheap when the
    // finger isn't on the panel.
    static void tick();
};

}  // namespace jarvis::app

#pragma once

// AudioPlayer — Phase 7 cloud-TTS playback.
//
// Wraps the CoreS3's built-in I2S amplifier (M5.Speaker) with an MP3
// decoder from ESP8266Audio. Plays a buffered MP3 (typically 10–60 KB
// from net/TtsClient) through M5.Speaker's virtual-channel mixer.
//
// Usage from main.cpp:
//   AudioPlayer::begin();             // once, after M5.Speaker.begin()
//   AudioPlayer::setOnPlayDone([]{ ... });
//   ...
//   AudioPlayer::play(std::move(mp3));  // start playback (non-blocking)
//   ...
//   AudioPlayer::tick();              // every loop() iteration
//
// Single-track: a play() while another is in flight stops the previous
// track first. The FSM never asks for two simultaneous TTS playbacks
// anyway (CLAUDE.md "TTS and ASR cannot run simultaneously").

#include <Arduino.h>
#include <functional>

#include "../net/TtsClient.h"

namespace jarvis::hal {

class AudioPlayer {
public:
    using OnDoneCb = std::function<void()>;

    // Initialise M5.Speaker (if not already begun) and configure the
    // virtual channel + sample rate. Idempotent.
    static bool begin();

    // True iff begin() succeeded.
    static bool ok();

    // Hand off an MP3 buffer for playback. Takes ownership. Stops any
    // currently-playing track first. Returns false if not begun, or if
    // the buffer is empty.
    static bool play(jarvis::net::Mp3Buffer mp3);

    // Stop and release the current track. Safe to call when nothing is
    // playing. Does NOT fire the OnDone callback — that fires only on
    // a natural end of playback.
    static void stop();

    // True while the decoder is still feeding samples to the speaker.
    static bool isPlaying();

    // Drive the decoder. Call from loop() every iteration. Cheap when
    // nothing is playing.
    static void tick();

    // Register a one-shot callback that fires when the current track
    // ends naturally. Used by the FSM to drive SPEAKING → IDLE the
    // same way the existing melotts setOnSpeakDone() does.
    static void setOnPlayDone(OnDoneCb cb);
};

}  // namespace jarvis::hal

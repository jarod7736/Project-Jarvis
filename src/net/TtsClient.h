#pragma once

// TtsClient — Phase 7 cloud-TTS HTTP client.
//
// Sends a POST to OpenAI's /v1/audio/speech (or ElevenLabs equivalent),
// receives an MP3 audio body, and returns it as an in-memory buffer
// allocated from PSRAM. Caller is responsible for free()-ing the buffer
// (or letting the unique_ptr drop) and for handing it to AudioPlayer.
//
// MVP is buffered — full download before playback. Streaming
// (chunk-download → ring buffer → I2S so the user hears the first
// syllable while bytes are still in flight) is a follow-up enhancement
// once we know the buffered path works at all.
//
// Failure modes: missing api_key, HTTP error, oversized response,
// network unreachable, JSON-error from the provider. All return a null
// result; caller falls back to melotts.

#include <Arduino.h>
#include <memory>

namespace jarvis::net {

struct Mp3Buffer {
    // Heap-allocated PSRAM buffer holding the MP3 bytes. Released by
    // operator delete[] when the unique_ptr drops, so callers don't
    // have to remember to call free().
    std::unique_ptr<uint8_t[]> data;
    size_t                     length = 0;

    bool empty() const { return length == 0 || !data; }
};

class TtsClient {
public:
    // True iff at least an api_key is provisioned. Provider/voice/model
    // all fall back to defaults from config.h, so they aren't required
    // for routing — just the key.
    static bool isConfigured();

    // Synthesise `text` to MP3 via the configured provider. Blocks for
    // up to kTtsHttpTimeoutMs. Returns an empty buffer on any failure;
    // a Serial.printf at every failure path makes diagnosis easy.
    // Caller (LLMModule::speak()) falls back to melotts on empty.
    static Mp3Buffer synthesize(const String& text);
};

}  // namespace jarvis::net

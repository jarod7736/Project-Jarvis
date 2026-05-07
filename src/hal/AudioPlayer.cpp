#include "AudioPlayer.h"

#include <M5Unified.h>

// ESP8266Audio — runs on ESP32-S3 despite the name. See platformio.ini.
#include <AudioFileSource.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

#include "../app/NVSConfig.h"

namespace jarvis::hal {

namespace {

// ── Custom AudioOutput → M5.Speaker bridge ────────────────────────────────
// Pattern lifted from M5Unified's MP3_with_ESP8266Audio example. Decoded
// PCM samples (stereo int16) are buffered in a triple buffer and pushed
// to M5.Speaker.playRaw() in chunks. Triple buffering smooths the audio
// over the decoder's irregular frame timing without growing latency.
class AudioOutputM5Speaker : public AudioOutput {
public:
    AudioOutputM5Speaker(m5::Speaker_Class* spk, uint8_t virtual_channel = 0)
        : spk_(spk), vch_(virtual_channel) {}

    bool begin() override { return true; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (idx_ < kBufSize) {
            buf_[bank_][idx_]     = sample[0];
            buf_[bank_][idx_ + 1] = sample[1];
            idx_ += 2;
            return true;
        }
        flush();
        return false;
    }

    void flush() override {
        if (idx_ == 0) return;
        spk_->playRaw(buf_[bank_], idx_, hertz, /*stereo=*/true,
                      /*repeat=*/1, vch_);
        bank_ = (bank_ + 1) % 3;
        idx_  = 0;
    }

    bool stop() override {
        flush();
        spk_->stop(vch_);
        return true;
    }

private:
    m5::Speaker_Class* spk_;
    uint8_t            vch_;
    static constexpr size_t kBufSize = 1536;
    int16_t            buf_[3][kBufSize] = {};
    size_t             bank_ = 0;
    size_t             idx_  = 0;
};

// ── In-memory AudioFileSource ─────────────────────────────────────────────
// ESP8266Audio's bundled sources are file/HTTP/PROGMEM. We've already
// downloaded the MP3 into a heap buffer (PSRAM), so the lightest path is
// a tiny adapter that streams from that buffer. Implements only what
// AudioGeneratorMP3 actually calls: read, seek, getPos, getSize,
// isOpen, close.
class AudioFileSourceMemory : public AudioFileSource {
public:
    AudioFileSourceMemory(const uint8_t* data, size_t len)
        : data_(data), len_(len), pos_(0), open_(true) {}

    bool open(const char* /*filename*/) override { return open_ = true; }
    bool close() override                         { open_ = false; return true; }
    bool isOpen() override                        { return open_; }
    uint32_t getSize() override                   { return len_; }
    uint32_t getPos() override                    { return pos_; }

    uint32_t read(void* dst, uint32_t bytes) override {
        if (pos_ >= len_) return 0;
        size_t n = (size_t)bytes;
        if (n > len_ - pos_) n = len_ - pos_;
        memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return n;
    }

    bool seek(int32_t off, int dir) override {
        size_t newpos;
        if (dir == SEEK_SET)      newpos = (size_t)off;
        else if (dir == SEEK_CUR) newpos = pos_ + off;
        else if (dir == SEEK_END) newpos = len_ + off;  // off is negative
        else return false;
        if (newpos > len_) return false;
        pos_ = newpos;
        return true;
    }

private:
    const uint8_t* data_;
    size_t         len_;
    size_t         pos_;
    bool           open_;
};

// ── Player state ──────────────────────────────────────────────────────────
constexpr uint8_t  kVirtualChannel = 0;
constexpr uint32_t kSampleRate     = 96000;  // matches the M5 mp3 example

bool                                  g_ok       = false;
AudioOutputM5Speaker*                 g_out      = nullptr;
AudioGeneratorMP3*                    g_mp3      = nullptr;
AudioFileSourceMemory*                g_src      = nullptr;
jarvis::net::Mp3Buffer                g_buffer;     // owns the MP3 bytes
AudioPlayer::OnDoneCb                 g_onDone;
bool                                  g_running  = false;

void teardown_track() {
    if (g_mp3 && g_mp3->isRunning()) g_mp3->stop();
    if (g_src) { g_src->close(); delete g_src; g_src = nullptr; }
    delete g_mp3; g_mp3 = nullptr;
    g_buffer = jarvis::net::Mp3Buffer{};  // releases PSRAM
    g_running = false;
}

}  // namespace

bool AudioPlayer::begin() {
    if (g_ok) return true;

    // Bring up M5.Speaker if not already running. M5.begin() in main.cpp
    // initialises the AXP2101 power rails but doesn't auto-start the
    // speaker on every CoreS3 firmware revision. Calling begin() again
    // is harmless if it's already up.
    auto cfg = M5.Speaker.config();
    cfg.sample_rate = kSampleRate;
    M5.Speaker.config(cfg);
    if (!M5.Speaker.begin()) {
        Serial.println("[AudioPlayer] M5.Speaker.begin failed");
        return false;
    }

    g_out = new AudioOutputM5Speaker(&M5.Speaker, kVirtualChannel);
    if (!g_out) {
        Serial.println("[AudioPlayer] alloc AudioOutputM5Speaker failed");
        return false;
    }
    Serial.printf("[AudioPlayer] ready (sample_rate=%u, vch=%u)\n",
                  (unsigned)kSampleRate, (unsigned)kVirtualChannel);
    g_ok = true;

    // Pick up the NVS-stored volume immediately. Default is 70 (per
    // ConfigSchema). Setting after g_ok so a stale call before begin
    // succeeded couldn't accidentally hit a half-initialised speaker.
    setVolume(jarvis::NVSConfig::getTtsVolume());
    return true;
}

void AudioPlayer::setVolume(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    // M5.Speaker.setVolume takes 0–255. Linear map: 0% → silent,
    // 100% → full output. The amplifier on CoreS3 saturates well
    // before 255 so values above ~200 are mostly cosmetic, but the
    // schema's 0–100 range is the user-facing knob and we honor it
    // proportionally rather than imposing our own ceiling.
    uint8_t m5_vol = static_cast<uint8_t>((pct * 255) / 100);
    M5.Speaker.setVolume(m5_vol);
}

bool AudioPlayer::ok() { return g_ok; }

bool AudioPlayer::play(jarvis::net::Mp3Buffer mp3) {
    if (!g_ok) {
        Serial.println("[AudioPlayer] play() before begin()");
        return false;
    }
    if (mp3.empty()) {
        Serial.println("[AudioPlayer] play() got empty buffer");
        return false;
    }

    teardown_track();
    g_buffer = std::move(mp3);
    g_src    = new AudioFileSourceMemory(g_buffer.data.get(), g_buffer.length);
    g_mp3    = new AudioGeneratorMP3();
    if (!g_src || !g_mp3) {
        Serial.println("[AudioPlayer] alloc decoder failed");
        teardown_track();
        return false;
    }

    if (!g_mp3->begin(g_src, g_out)) {
        Serial.println("[AudioPlayer] mp3.begin failed");
        teardown_track();
        return false;
    }
    g_running = true;
    Serial.printf("[AudioPlayer] playing %u-byte MP3\n",
                  (unsigned)g_buffer.length);
    return true;
}

void AudioPlayer::stop() {
    if (!g_running) return;
    Serial.println("[AudioPlayer] stop()");
    teardown_track();
}

bool AudioPlayer::isPlaying() {
    return g_running && g_mp3 && g_mp3->isRunning();
}

void AudioPlayer::tick() {
    if (!g_running || !g_mp3) return;
    if (g_mp3->isRunning()) {
        if (!g_mp3->loop()) {
            // loop() returns false on natural end-of-stream OR a fatal
            // decoder error. Treat both as "done" — fire callback,
            // tear down. Distinguishing them isn't useful here; the
            // FSM transitions out of SPEAKING either way.
            g_mp3->stop();
            // Fall through to the done branch.
        }
    }
    if (!g_mp3->isRunning()) {
        Serial.println("[AudioPlayer] track done");
        teardown_track();
        if (g_onDone) g_onDone();
    }
}

void AudioPlayer::setOnPlayDone(OnDoneCb cb) { g_onDone = std::move(cb); }

}  // namespace jarvis::hal

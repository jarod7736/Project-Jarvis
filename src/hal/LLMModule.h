#pragma once

#include <Arduino.h>
#include <functional>

class M5ModuleLLM;
class HardwareSerial;

namespace jarvis::hal {

// Phase 2 voice-loop wrapper around M5ModuleLLM.
//
// Replaces the bundled M5ModuleLLM_VoiceAssistant preset which doesn't work
// end-to-end against current StackFlow firmware (v1.3) — see PLAN.md Phase 1
// retro. We hand-wire the audio→KWS→ASR→melotts chain ourselves and poll the
// raw responseMsgList that the lib exposes.
//
// The LLM unit is intentionally NOT initialized here — Phase 2 is an echo
// stub (transcript → TTS), not a full conversational loop. Phase 5 adds LLM.
//
// Threading: single-threaded. update() must be called from loop(). All
// callbacks fire from update() — they may set flags, but must not call speak()
// or block.
class LLMModule {
public:
    LLMModule();
    ~LLMModule();
    LLMModule(const LLMModule&)            = delete;
    LLMModule& operator=(const LLMModule&) = delete;

    // Brings up UART, checks connection, sends explicit audio.work, and runs
    // setup for kws / asr / melotts. Blocking (~20-30s on cold start due to
    // model loads). Returns false on any setup failure; check getLastError()
    // for context.
    bool begin(HardwareSerial* serial);

    // Pump UART. Call every loop() iteration — it polls the lib's response
    // list and fires whichever callbacks are registered.
    void update();

    // Initiate TTS playback. Non-blocking. The on_speak_done callback fires
    // after a duration estimated from text length (real TTS-done detection
    // is a Phase 5/6 task).
    void speak(const String& text);

    // Callback registration. Pattern: callbacks set flags only — they must
    // not transition state, call speak(), or block.
    using WakeCb       = std::function<void()>;
    using TranscriptCb = std::function<void(const String& text, bool isFinal)>;
    using SpeakDoneCb  = std::function<void()>;
    void setOnWake(WakeCb cb)             { on_wake_       = std::move(cb); }
    void setOnTranscript(TranscriptCb cb) { on_transcript_ = std::move(cb); }
    void setOnSpeakDone(SpeakDoneCb cb)   { on_speak_done_ = std::move(cb); }

    // Phase 5: synchronous Qwen inference. Sends the prompt as UART input
    // (we set the LLM's input source to "llm.utf-8" at setup time, NOT
    // chained from ASR — the host owns routing). Blocks for up to
    // `timeoutMs` waiting for the final isFinish=true frame, then returns
    // the accumulated response. Empty string on failure.
    //
    // Caller must show "Thinking..." on display BEFORE calling — loop()
    // stalls during the wait, including module_->update() callbacks.
    String queryLlm(const String& prompt, uint32_t timeoutMs = 8000);

    // Diagnostics
    const String& getFwVersion()  const { return fw_version_; }
    const String& getLastError()  const { return last_error_; }
    bool          isReady()       const { return ready_; }
    bool          hasTts()        const { return melotts_work_id_.length() > 0; }
    bool          hasLlm()        const { return llm_work_id_.length() > 0; }

private:
    bool sendAudioWork();             // explicit start, retro fallback
    bool setupKws();
    bool setupAsr();
    bool setupLlm();                  // Qwen 0.5B for intent routing (Phase 5)
    bool setupMelottsWithLongTimeout(); // 60s wait, bypasses lib's 15s ceiling
    void dumpAvailableModels();         // sys.lsmode dump for diagnostics

    M5ModuleLLM* module_;             // owned, opaque to header (forward decl)
    String       fw_version_;
    String       last_error_;
    bool         ready_ = false;

    String       kws_work_id_;
    String       asr_work_id_;
    String       llm_work_id_;
    String       melotts_work_id_;

    // ASR streaming arrives as fragments; we accumulate so callers see the
    // full transcript on each callback rather than just the latest delta.
    String       asr_accum_;

    // SPEAKING duration estimate — see config::kSpeakBaseMs / kSpeakMsPerChar.
    bool         speaking_         = false;
    uint32_t     speak_done_at_ms_ = 0;

    WakeCb       on_wake_;
    TranscriptCb on_transcript_;
    SpeakDoneCb  on_speak_done_;
};

}  // namespace jarvis::hal

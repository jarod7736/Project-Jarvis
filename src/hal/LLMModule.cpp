#include "LLMModule.h"

#include <ArduinoJson.h>
#include <M5ModuleLLM.h>

#include "../app/NVSConfig.h"
#include "../config.h"
#include "../net/TtsClient.h"
#include "../prompts/intent_prompt.h"
#include "AudioPlayer.h"

namespace jarvis::hal {

namespace {

// Build a setup JSON for the melotts unit and send it via the lib's raw cmd
// path so we can wait longer than the lib's hardcoded 15s ceiling. The
// model name varies by firmware version: v1.6+ uses "melotts-en-default"
// (per api_melotts.cpp:38 in the lib); older firmware used "melotts_zh-cn".
String buildMelottsSetupCmd(const String& request_id, const char* model) {
    JsonDocument doc;
    doc["request_id"]                = request_id;
    doc["work_id"]                   = "melotts";
    doc["action"]                    = "setup";
    doc["object"]                    = "melotts.setup";
    auto data                        = doc["data"].to<JsonObject>();
    data["model"]                    = model;
    data["response_format"]          = "sys.pcm";
    auto input                       = data["input"].to<JsonArray>();
    input.add("tts.utf-8.stream");
    data["enoutput"]                 = false;
    data["enaudio"]                  = true;

    String out;
    serializeJson(doc, out);
    return out;
}

// audio.work: explicit "start the mic capture pipeline" command. The v1.0.0
// API spec describes <unit>.work as the post-setup start action; the v1.3
// firmware deprecates audio.setup but doesn't actually start the pipeline
// (Phase 1 retro). Sending this directly is the workaround.
String buildAudioWorkCmd(const String& request_id) {
    JsonDocument doc;
    doc["request_id"]                = request_id;
    doc["work_id"]                   = "audio.1000";  // predictable per v1.0.0 spec
    doc["action"]                    = "work";

    String out;
    serializeJson(doc, out);
    return out;
}

// audio.exit: stop the capture pipeline. Sent raw (not via ApiAudio::exit)
// so the request shape stays symmetric with sendAudioWork() above and so
// we can name the request_id ourselves for response correlation.
String buildAudioExitCmd(const String& request_id) {
    JsonDocument doc;
    doc["request_id"] = request_id;
    doc["work_id"]    = "audio.1000";
    doc["action"]     = "exit";
    String out;
    serializeJson(doc, out);
    return out;
}

// audio.setup with caller-supplied capVolume. Builds the same payload
// ApiAudio::setup() would, but raw so the boot path and the live-update
// path stay parallel — and so applyMicGain() doesn't fight the lib's
// hardcoded 5s response wait when audio.setup is silently dropped by
// the v1.3 firmware.
String buildAudioSetupCmd(const String& request_id, float capVolume) {
    JsonDocument doc;
    doc["request_id"]         = request_id;
    doc["work_id"]            = "audio";
    doc["action"]             = "setup";
    doc["object"]             = "audio.setup";
    doc["data"]["capcard"]    = 0;
    doc["data"]["capdevice"]  = 0;
    doc["data"]["capVolume"]  = capVolume;
    doc["data"]["playcard"]   = 0;
    doc["data"]["playdevice"] = 1;
    doc["data"]["playVolume"] = 0.15f;  // Speaker volume lives on the host's
                                        // own M5.Speaker path (AudioPlayer)
                                        // — this value is unused by the
                                        // current pipeline but must be
                                        // present in the payload.
    String out;
    serializeJson(doc, out);
    return out;
}

}  // namespace

LLMModule::LLMModule()
    : module_(new M5ModuleLLM()) {}

LLMModule::~LLMModule() {
    delete module_;
}

bool LLMModule::begin(HardwareSerial* serial) {
    if (!module_->begin(serial)) {
        last_error_ = "module_llm.begin() failed";
        return false;
    }

    // Block until the LLM module daemon is up (sys.ping succeeds). Cold boot
    // of the AX630C takes a few seconds.
    Serial.print("[LLMModule] waiting for connection");
    uint32_t start = millis();
    while (!module_->checkConnection()) {
        Serial.print('.');
        if (millis() - start > 30000) {
            Serial.println(" TIMEOUT");
            last_error_ = "checkConnection timeout";
            return false;
        }
        delay(500);
    }
    Serial.println(" OK");

    // sys.version isn't in the public docs but the v1.7.0 lib exposes it.
    fw_version_ = module_->sys.version();
    Serial.printf("[LLMModule] StackFlow FW: %s (pinned: %s)\n",
                  fw_version_.c_str(), config::kStackflowFwPin);
    if (fw_version_ != config::kStackflowFwPin) {
        Serial.printf("[LLMModule] WARNING: FW version drift — re-test the UART path.\n");
    }

    // Reset stackflow service to clear any chain left behind by a previous
    // boot. The AX630C runs Linux and daemon state persists across CoreS3
    // resets — without this, a stale VoiceAssistant chain shadows our setup
    // and the host never sees KWS/ASR events.
    Serial.println("[LLMModule] sys.reset (clearing stale chain)...");
    int reset_ret = module_->sys.reset(true);
    Serial.printf("[LLMModule] sys.reset returned %d\n", reset_ret);
    // sys.reset(true) waits for the SYS daemon's "reset over" but other
    // daemons (audio, kws, asr, llm, melotts) keep loading after that. The
    // melotts daemon in particular loads a large lexicon (~16s wall-clock)
    // and isn't ready to respond to setup until well after sys.reset
    // returns. 3s is the empirically-comfortable buffer; 500ms wasn't.
    delay(3000);

    dumpAvailableModels();

    if (!sendAudioWork())                  return false;
    if (!setupKws())                       return false;
    if (!setupAsr())                       return false;

    // LLM (Qwen) is allowed to fail in Phase 5 too — IntentRouter will
    // fall back to the hardcoded CommandHandler path when hasLlm() is false.
    if (!setupLlm()) {
        Serial.printf("[LLMModule] WARNING: llm unavailable (%s). "
                      "Intent routing will fall back to keyword table.\n",
                      last_error_.c_str());
        last_error_ = "";
    }

    // TTS setup is allowed to fail too — display-only echo fallback.
    if (!setupMelottsWithLongTimeout()) {
        Serial.printf("[LLMModule] WARNING: melotts unavailable (%s). "
                      "Continuing with display-only echo.\n",
                      last_error_.c_str());
        last_error_ = "";
    }

    ready_ = true;
    Serial.println("[LLMModule] voice loop ready");
    return true;
}

void LLMModule::dumpAvailableModels() {
    // sys.lsmode isn't exposed on ApiSys — send raw and log the response so
    // we can see exactly what models the firmware has. One-shot diagnostic;
    // safe to leave in for the lifetime of Phase 2.
    module_->msg.responseMsgList.clear();
    JsonDocument doc;
    doc["request_id"] = "lsmode";
    doc["work_id"]    = "sys";
    doc["action"]     = "lsmode";
    String cmd;
    serializeJson(doc, cmd);
    module_->msg.sendCmd(cmd.c_str());

    const uint32_t deadline = millis() + 3000;
    while (millis() < deadline) {
        module_->update();
        for (auto& m : module_->msg.responseMsgList) {
            JsonDocument resp;
            if (deserializeJson(resp, m.raw_msg)) continue;
            if (resp["request_id"].as<String>() != "lsmode") continue;

            Serial.println("[LLMModule] sys.lsmode →");
            JsonArray arr = resp["data"].as<JsonArray>();
            for (auto entry : arr) {
                Serial.printf("  %-8s  %s\n",
                              entry["type"].as<const char*>(),
                              entry["model"].as<const char*>());
            }
            module_->msg.responseMsgList.clear();
            return;
        }
        delay(20);
    }
    Serial.println("[LLMModule] sys.lsmode timed out (continuing)");
}

bool LLMModule::sendAudioWork() {
    // Fire-and-forget. If audio is already working (per the v1.3 deprecation
    // claim of auto-config) the daemon returns MODULE_ALREADY_WORKING which is
    // fine — we don't need to read the response.
    String cmd = buildAudioWorkCmd("audio_work");
    module_->msg.sendCmd(cmd.c_str());
    Serial.println("[LLMModule] sent audio.work");
    return true;
}

bool LLMModule::setupKws() {
    m5_module_llm::ApiKwsSetupConfig_t cfg;
    cfg.kws       = config::kWakeWord;
    cfg.input     = {"sys.pcm"};
    cfg.enoutput  = true;
    kws_work_id_  = module_->kws.setup(cfg, "kws_setup", "en_US");
    if (kws_work_id_.length() == 0) {
        last_error_ = "kws.setup failed (timeout or refused)";
        return false;
    }
    Serial.printf("[LLMModule] kws_work_id=%s wake=%s\n",
                  kws_work_id_.c_str(), config::kWakeWord);
    return true;
}

bool LLMModule::setupAsr() {
    m5_module_llm::ApiAsrSetupConfig_t cfg;
    cfg.input          = {"sys.pcm", kws_work_id_};
    cfg.enoutput       = true;
    cfg.rule1          = config::kAsrRule1;
    cfg.rule2          = config::kAsrRule2;
    cfg.rule3          = config::kAsrRule3;
    asr_work_id_       = module_->asr.setup(cfg, "asr_setup", "en_US");
    if (asr_work_id_.length() == 0) {
        last_error_ = "asr.setup failed";
        return false;
    }
    Serial.printf("[LLMModule] asr_work_id=%s\n", asr_work_id_.c_str());
    return true;
}

bool LLMModule::setupLlm() {
    // The M5 firmware expects the system prompt at setup time and only the
    // user query at inference time. Sending the full prompt+query at
    // inference (as a Phase 5 first attempt did) makes Qwen treat it as
    // completion fodder and produce unrelated text.
    m5_module_llm::ApiLlmSetupConfig_t cfg;
    cfg.input           = {"llm.utf-8"};
    cfg.response_format = "llm.utf-8.stream";
    cfg.enoutput        = true;
    cfg.max_token_len   = 127;  // PLAN.md cap; matches the API doc default
    // PROGMEM read into the config's String. The intent prompt is large
    // (~1KB) and we only pay this once at boot.
    cfg.prompt = (const __FlashStringHelper*)jarvis::prompts::kIntentPrompt;
    llm_work_id_ = module_->llm.setup(cfg, "llm_setup");
    if (llm_work_id_.length() == 0) {
        last_error_ = "llm.setup failed";
        return false;
    }
    Serial.printf("[LLMModule] llm_work_id=%s (system prompt: %u chars)\n",
                  llm_work_id_.c_str(), (unsigned)cfg.prompt.length());
    return true;
}

String LLMModule::queryLlm(const String& prompt, uint32_t timeoutMs) {
    if (!hasLlm()) return String();

    module_->msg.responseMsgList.clear();
    const String request_id = "llm_query";
    int rc = module_->llm.inference(llm_work_id_, prompt, request_id);
    if (rc != MODULE_LLM_OK) {
        Serial.printf("[LLMModule] llm.inference returned %d\n", rc);
        return String();
    }

    String accum;
    accum.reserve(256);
    const uint32_t deadline = millis() + timeoutMs;

    while ((int32_t)(millis() - deadline) < 0) {
        module_->update();
        for (auto& m : module_->msg.responseMsgList) {
            if (m.work_id != llm_work_id_) continue;
            JsonDocument doc;
            if (deserializeJson(doc, m.raw_msg)) continue;
            if (doc["request_id"].as<String>() != request_id) continue;

            String delta = doc["data"]["delta"].as<String>();
            // Per the v1.0.0 spec, llm streaming `delta` is per-token; unlike
            // ASR's full-snapshot semantics. Verified by reading existing
            // main.cpp Phase 1 code which accumulates fragments.
            accum += delta;

            bool fin = doc["data"]["finish"] | false;
            if (fin) {
                module_->msg.responseMsgList.clear();
                return accum;
            }
        }
        module_->msg.responseMsgList.clear();
        delay(10);
    }
    Serial.printf("[LLMModule] queryLlm deadline exceeded (got %u chars)\n",
                  (unsigned)accum.length());
    return accum;  // partial — still better than empty
}

// Try the lib's melotts.setup path. The lib forces the model name based on
// llm_version + language; on v1.6+ with en_US that's "melotts-en-default".
bool tryMelottsLib(M5ModuleLLM* module, String& work_id_out, int attempt) {
    Serial.printf("[LLMModule] melotts.setup attempt %d (lib path, 15s)\n", attempt);
    work_id_out = module->melotts.setup({}, "melotts_setup", "en_US");
    return work_id_out.length() > 0;
}

// Raw-JSON setup with caller-supplied model name and 60s wait. Used when
// the lib path keeps returning empty.
bool tryMelottsRaw(M5ModuleLLM* module, const char* model,
                   String& work_id_out, String& err_out) {
    module->msg.responseMsgList.clear();
    String request_id = "melotts_setup_raw_";
    request_id += model;
    const String cmd = buildMelottsSetupCmd(request_id, model);
    module->msg.sendCmd(cmd.c_str());
    Serial.printf("[LLMModule] raw melotts.setup model=%s\n", model);

    const uint32_t deadline = millis() + config::kMelottsSetupMs;
    while (millis() < deadline) {
        module->update();
        for (auto& m : module->msg.responseMsgList) {
            JsonDocument doc;
            if (deserializeJson(doc, m.raw_msg)) continue;
            if (doc["request_id"].as<String>() != request_id) continue;

            int code = doc["error"]["code"] | -99;
            if (code != 0) {
                err_out = String("error code=") + code + " (model=" + model + ")";
                module->msg.responseMsgList.clear();
                return false;
            }
            work_id_out = doc["work_id"].as<String>();
            module->msg.responseMsgList.clear();
            return true;
        }
        delay(20);
    }
    err_out = String("deadline exceeded (model=") + model + ")";
    return false;
}

bool LLMModule::setupMelottsWithLongTimeout() {
    // Attempt 1: lib's standard call. Often succeeds when the daemon is warm.
    if (tryMelottsLib(module_, melotts_work_id_, 1)) {
        Serial.printf("[LLMModule] melotts_work_id=%s\n", melotts_work_id_.c_str());
        return true;
    }

    // The -21 / "lib returned empty" outcomes are typically transient — the
    // melotts daemon hasn't fully warmed up after sys.reset. Wait, retry.
    Serial.println("[LLMModule] lib attempt 1 failed; warming up 3s and retrying");
    delay(3000);
    if (tryMelottsLib(module_, melotts_work_id_, 2)) {
        Serial.printf("[LLMModule] melotts_work_id=%s\n", melotts_work_id_.c_str());
        return true;
    }

    // Last-ditch: raw-JSON with the v1.6+ model name, 60s wait. Then try the
    // older zh-cn name in case the device only has that one for some reason.
    String err1, err2;
    if (tryMelottsRaw(module_, "melotts-en-default", melotts_work_id_, err1)) {
        Serial.printf("[LLMModule] melotts_work_id=%s (raw en-default)\n",
                      melotts_work_id_.c_str());
        return true;
    }
    Serial.printf("[LLMModule] raw en-default: %s\n", err1.c_str());

    if (tryMelottsRaw(module_, "melotts_zh-cn", melotts_work_id_, err2)) {
        Serial.printf("[LLMModule] melotts_work_id=%s (raw zh-cn)\n",
                      melotts_work_id_.c_str());
        return true;
    }
    Serial.printf("[LLMModule] raw zh-cn: %s\n", err2.c_str());

    last_error_ = "melotts.setup all paths failed";
    return false;
}

void LLMModule::update() {
    module_->update();

    for (auto& m : module_->msg.responseMsgList) {
        if (kws_work_id_.length() && m.work_id == kws_work_id_) {
            Serial.printf("[LLMModule] KWS event raw=%s\n", m.raw_msg.c_str());
            if (on_wake_) on_wake_();
            continue;
        }

        if (asr_work_id_.length() && m.work_id == asr_work_id_) {
            // Each ASR streaming msg's `data.delta` is the FULL running
            // transcript hypothesis up to that point — not an incremental
            // fragment. Replace, don't accumulate. (Verified empirically on
            // StackFlow v1.6: accumulating produced "what what what what
            // happening...".) Use .trim() to drop the leading space the
            // firmware tends to prepend.
            JsonDocument doc;
            if (deserializeJson(doc, m.raw_msg)) continue;

            String text = doc["data"]["delta"].as<String>();
            bool   fin  = doc["data"]["finish"] | false;
            text.trim();

            if (on_transcript_) on_transcript_(text, fin);
            continue;
        }
    }
    module_->msg.responseMsgList.clear();

    // Phase 2 SPEAKING-done estimate. The lib's TTS unit doesn't expose a
    // playback-finished event we can hook (the audio plays on the LLM
    // Module's speaker, out-of-band from UART). Estimate from text length.
    if (speaking_ && (int32_t)(millis() - speak_done_at_ms_) >= 0) {
        speaking_ = false;
        if (on_speak_done_) on_speak_done_();
    }
}

void LLMModule::speak(const String& text) {
    if (!ready_) return;

    // Guard against re-entry. The FSM should never call speak() while
    // already SPEAKING — but if it ever does (callback flag race, partial
    // tick), we want a single log line instead of two overlapping melotts
    // inferences. Investigating "Hi twice on wake" — instrumenting first.
    if (speaking_) {
        Serial.printf("[LLMModule] speak() ignored — already speaking. "
                      "incoming=\"%s\"\n", text.c_str());
        return;
    }

    Serial.printf("[LLMModule] speak(\"%s\") len=%u\n",
                  text.c_str(), (unsigned)text.length());

    // Phase 7 cloud-TTS path. tts_provider != "melotts" + an api_key
    // present + AudioPlayer ready = try cloud first. Failure on any
    // step falls through to the melotts path below — no break in
    // user-facing behavior.
    String tts_provider = jarvis::NVSConfig::getTtsProvider();
    if (!tts_provider.equalsIgnoreCase("melotts") &&
        jarvis::net::TtsClient::isConfigured() &&
        jarvis::hal::AudioPlayer::ok()) {
        jarvis::net::Mp3Buffer mp3 = jarvis::net::TtsClient::synthesize(text);
        if (!mp3.empty() && jarvis::hal::AudioPlayer::play(std::move(mp3))) {
            // Speaking is now driven by AudioPlayer; speak_done_at_ms_
            // gets a far-future sentinel so update()'s estimate-based
            // fallback timer doesn't fire while audio is still playing.
            // finishSpeaking() (wired via AudioPlayer::setOnPlayDone in
            // main.cpp) flips us back to !speaking_ when the buffer
            // drains.
            speaking_         = true;
            speak_done_at_ms_ = millis() + 60UL * 60UL * 1000UL;
            Serial.println("[LLMModule] speak: cloud TTS in flight");
            return;
        }
        Serial.println("[LLMModule] cloud TTS failed; falling back to melotts");
    }

    if (melotts_work_id_.length() == 0) {
        // Phase 2 fallback: TTS unavailable — show on display, schedule
        // SPEAKING→IDLE on a short timer so the FSM still exercises the
        // transition. Audio echo will work once TTS lands.
        Serial.printf("[LLMModule] (no TTS) would speak: \"%s\"\n", text.c_str());
        speaking_         = true;
        speak_done_at_ms_ = millis() + 1500;
        return;
    }

    // The Module-LLM TTS API requires a terminal period for English text.
    String body = text;
    if (body.length() == 0) body = ".";
    if (body[body.length() - 1] != '.' && body[body.length() - 1] != '?'
                                       && body[body.length() - 1] != '!') {
        body += '.';
    }

    module_->melotts.inference(melotts_work_id_, body, 0, "tts_inference");

    speaking_         = true;
    speak_done_at_ms_ = millis()
                      + config::kSpeakBaseMs
                      + (uint32_t)body.length() * config::kSpeakMsPerChar;
}

void LLMModule::finishSpeaking() {
    if (!speaking_) return;
    speaking_ = false;
    if (on_speak_done_) on_speak_done_();
}

bool LLMModule::applyMicGain(int pct) {
    if (!ready_) {
        Serial.println("[LLMModule] applyMicGain: not ready");
        return false;
    }
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    // 0..100% → 0.0..2.0 in StackFlow capVolume terms. See header for
    // rationale (50% = unity, 100% = 2x, well below the 10.0 spec ceiling
    // where the input clips into noise on this mic).
    const float capVolume = (float)pct / 50.0f;
    Serial.printf("[LLMModule] applyMicGain(%d%%) -> capVolume=%.2f\n",
                  pct, capVolume);

    // ── 1. Tear down consumers of sys.pcm first ──────────────────────────
    // Order matters: ASR is downstream of KWS (input chain is sys.pcm →
    // kws_work_id → asr). Exiting in the same order avoids the daemon
    // emitting "input dead" errors for the still-bound downstream unit.
    if (asr_work_id_.length()) {
        module_->asr.exit(asr_work_id_, "asr_exit_mic");
        Serial.printf("[LLMModule] asr.exit(%s)\n", asr_work_id_.c_str());
        asr_work_id_ = "";
    }
    if (kws_work_id_.length()) {
        module_->kws.exit(kws_work_id_, "kws_exit_mic");
        Serial.printf("[LLMModule] kws.exit(%s)\n", kws_work_id_.c_str());
        kws_work_id_ = "";
    }

    // ── 2. Cycle the audio unit with the new capVolume ───────────────────
    // audio.exit then audio.setup. Both raw; the v1.3 firmware may drop
    // audio.setup silently (deprecation), in which case we proceed
    // optimistically — the next audio.work will start capture at whatever
    // capVolume the daemon chose for itself.
    {
        String cmd = buildAudioExitCmd("audio_exit_mic");
        module_->msg.sendCmd(cmd.c_str());
        Serial.println("[LLMModule] audio.exit");
        // Brief settle: give the daemon a moment to release the soundcard
        // before re-claiming it. Empirically the AX630C side needs ≥50 ms.
        delay(150);
    }
    {
        const String request_id = "audio_setup_mic";
        String cmd = buildAudioSetupCmd(request_id, capVolume);
        module_->msg.sendCmd(cmd.c_str());

        // Wait briefly for a response so we can distinguish "accepted",
        // "rejected", and "silently ignored" outcomes in the serial log.
        // The pipeline still gets re-armed in all three cases — only an
        // explicit error code from the daemon is treated as a failure.
        const uint32_t deadline = millis() + 2000;
        bool got_response = false;
        int  error_code   = 0;
        while (millis() < deadline) {
            module_->update();
            for (auto& m : module_->msg.responseMsgList) {
                JsonDocument doc;
                if (deserializeJson(doc, m.raw_msg)) continue;
                if (doc["request_id"].as<String>() != request_id) continue;
                error_code   = doc["error"]["code"] | -99;
                got_response = true;
                break;
            }
            module_->msg.responseMsgList.clear();
            if (got_response) break;
            delay(20);
        }
        if (!got_response) {
            Serial.println("[LLMModule] audio.setup: no response in 2s "
                           "(daemon likely treats it as no-op on this FW)");
        } else if (error_code != 0) {
            Serial.printf("[LLMModule] audio.setup: daemon returned error=%d "
                          "— continuing with re-arm anyway\n", error_code);
        } else {
            Serial.println("[LLMModule] audio.setup OK");
        }
    }

    // ── 3. Re-arm capture + KWS + ASR ────────────────────────────────────
    // sendAudioWork() is fire-and-forget (it would be on the boot path too)
    // so any "already working" race is swallowed by the daemon. Then setup
    // KWS + ASR in the original order, reusing the same builder functions
    // begin() uses — no behavioural drift between cold-boot and live-update.
    if (!sendAudioWork()) {
        // sendAudioWork currently can't return false, but keep the guard
        // for future readers who might add validation there.
        last_error_ = "applyMicGain: sendAudioWork failed";
        Serial.println("[LLMModule] applyMicGain: sendAudioWork failed");
        return false;
    }
    delay(150);

    if (!setupKws()) {
        Serial.printf("[LLMModule] applyMicGain: kws.setup failed: %s\n",
                      last_error_.c_str());
        return false;
    }
    if (!setupAsr()) {
        Serial.printf("[LLMModule] applyMicGain: asr.setup failed: %s\n",
                      last_error_.c_str());
        return false;
    }

    Serial.println("[LLMModule] applyMicGain: pipeline restored");
    return true;
}

}  // namespace jarvis::hal

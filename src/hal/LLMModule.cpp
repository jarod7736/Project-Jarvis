#include "LLMModule.h"

#include <ArduinoJson.h>
#include <M5ModuleLLM.h>

#include "../config.h"

namespace jarvis::hal {

namespace {

// Build a setup JSON for the melotts unit and send it via the lib's raw cmd
// path so we can wait longer than the lib's hardcoded 15s ceiling. Body
// matches ApiMelottsSetupConfig_t defaults — any future tuning happens here.
String buildMelottsSetupCmd(const String& request_id) {
    JsonDocument doc;
    doc["request_id"]                = request_id;
    doc["work_id"]                   = "melotts";
    doc["action"]                    = "setup";
    doc["object"]                    = "melotts.setup";
    auto data                        = doc["data"].to<JsonObject>();
    data["model"]                    = "melotts_zh-cn";  // covers EN+ZH despite the name
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
    delay(500);  // let daemons settle

    dumpAvailableModels();

    if (!sendAudioWork())                  return false;
    if (!setupKws())                       return false;
    if (!setupAsr())                       return false;

    // TTS setup is allowed to fail in Phase 2 — we log it and continue. The
    // FSM falls back to a display-only "speaking" path when hasTts() is false.
    // KWS+ASR validation gates can still be exercised end-to-end.
    if (!setupMelottsWithLongTimeout()) {
        Serial.printf("[LLMModule] WARNING: melotts unavailable (%s). "
                      "Continuing with display-only echo.\n",
                      last_error_.c_str());
        last_error_ = "";  // don't propagate as fatal
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

bool LLMModule::setupMelottsWithLongTimeout() {
    // First try the library's standard call. On v1.3 firmware its 15s timeout
    // may or may not be enough — if the daemon is fast on this build, this
    // succeeds and we save a lot of complexity. If it returns empty, fall
    // through to the raw-JSON workaround below.
    Serial.println("[LLMModule] melotts.setup (lib path, 15s)");
    melotts_work_id_ = module_->melotts.setup({}, "melotts_setup", "en_US");
    if (melotts_work_id_.length() > 0) {
        Serial.printf("[LLMModule] melotts_work_id=%s\n", melotts_work_id_.c_str());
        return true;
    }

    Serial.println("[LLMModule] lib path returned empty; trying raw 60s wait");
    module_->msg.responseMsgList.clear();
    const String request_id = "melotts_setup_long";
    const String cmd        = buildMelottsSetupCmd(request_id);
    module_->msg.sendCmd(cmd.c_str());

    const uint32_t deadline = millis() + config::kMelottsSetupMs;
    while (millis() < deadline) {
        module_->update();
        for (auto& m : module_->msg.responseMsgList) {
            JsonDocument doc;
            if (deserializeJson(doc, m.raw_msg)) continue;
            if (doc["request_id"].as<String>() != request_id) continue;

            int code = doc["error"]["code"] | -99;
            if (code != 0) {
                last_error_ = String("melotts.setup error code=") + code;
                module_->msg.responseMsgList.clear();
                return false;
            }
            melotts_work_id_ = doc["work_id"].as<String>();
            Serial.printf("[LLMModule] melotts_work_id=%s (raw path)\n",
                          melotts_work_id_.c_str());
            module_->msg.responseMsgList.clear();
            return true;
        }
        delay(20);
    }

    last_error_ = "melotts.setup deadline exceeded";
    return false;
}

void LLMModule::update() {
    module_->update();

    for (auto& m : module_->msg.responseMsgList) {
        if (kws_work_id_.length() && m.work_id == kws_work_id_) {
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

}  // namespace jarvis::hal

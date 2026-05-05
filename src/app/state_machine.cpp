#include "state_machine.h"

#include <Arduino.h>

#include "../config.h"
#include "../net/WiFiManager.h"
#include "IntentRouter.h"

namespace jarvis::app {

using jarvis::hal::DeviceState;
using jarvis::hal::Display;
using jarvis::hal::LLMModule;

namespace {

// All state lives in this anonymous namespace. Single-threaded — no atomics
// needed. Callbacks fire from LLMModule::update() which is called from
// loop(), same context as tickStateMachine().
LLMModule* g_module = nullptr;
DeviceState g_state = DeviceState::IDLE;

// Flags set by callbacks, drained by tickStateMachine().
bool   g_kws_fired       = false;
bool   g_transcript_dirty = false;
bool   g_asr_final       = false;
String g_transcript;
bool   g_speak_done      = false;

// Listening-window deadline. millis() value at which we give up on hearing
// speech and return to IDLE. 0 == not listening.
uint32_t g_listen_deadline_ms = 0;

void enterIdle() {
    g_state = DeviceState::IDLE;
    g_listen_deadline_ms = 0;
    Display::setStatus(DeviceState::IDLE);
    Serial.println("[FSM] -> IDLE");
}

void enterListening() {
    g_state = DeviceState::LISTENING;
    g_listen_deadline_ms = millis() + config::kListenTimeoutMs;
    // Drain stale flags from any prior session — without this, an ASR.final
    // that arrived during SPEAKING would trigger a phantom transition.
    g_asr_final        = false;
    g_transcript_dirty = false;
    g_transcript       = "";
    Display::setStatus(DeviceState::LISTENING);
    Display::showTranscript("");
    Serial.println("[FSM] -> LISTENING");
}

void enterTranscribing() {
    g_state = DeviceState::THINKING;  // shown as THINKING (yellow) on display
    Display::setStatus(DeviceState::THINKING);
    Serial.println("[FSM] -> TRANSCRIBING (THINKING)");
}

void enterSpeaking(const String& text) {
    g_state = DeviceState::SPEAKING;
    Display::setStatus(DeviceState::SPEAKING);
    Display::showResponse(text);
    Serial.printf("[FSM] -> SPEAKING (\"%s\")\n", text.c_str());
    g_module->speak(text);
}

}  // namespace

void stateMachineBegin(LLMModule* module) {
    g_module = module;

    g_module->setOnWake([]() { g_kws_fired = true; });

    g_module->setOnTranscript([](const String& text, bool isFinal) {
        g_transcript        = text;
        g_transcript_dirty  = true;
        if (isFinal) g_asr_final = true;
    });

    g_module->setOnSpeakDone([]() { g_speak_done = true; });

    enterIdle();
}

void tickStateMachine() {
    // Partial-transcript display refresh. Gate on LISTENING so stale ASR
    // events arriving after timeout/SPEAKING don't paint over a settled UI.
    // Also extend the LISTENING deadline on every fragment — the user is
    // clearly still speaking, so the 10s budget should mean "silence after
    // wake," not "total speech time."
    if (g_transcript_dirty && g_state == DeviceState::LISTENING) {
        g_transcript_dirty = false;
        Display::showTranscript(g_transcript);
        g_listen_deadline_ms = millis() + config::kListenTimeoutMs;
    } else if (g_transcript_dirty) {
        g_transcript_dirty = false;  // drop on the floor
    }

    switch (g_state) {
        case DeviceState::IDLE: {
            if (g_kws_fired) {
                g_kws_fired = false;
                enterListening();
            }
            break;
        }

        case DeviceState::LISTENING: {
            if (g_asr_final) {
                g_asr_final = false;
                String final_text = g_transcript;
                g_transcript = "";
                Serial.printf("[FSM] final transcript: \"%s\"\n", final_text.c_str());
                enterTranscribing();
                if (final_text.length() == 0) {
                    enterIdle();
                } else {
                    // Phase 5: route through Qwen IntentRouter. Display is
                    // already in THINKING from enterTranscribing(), so the
                    // CLAUDE.md "feedback-before-blocking" rule holds. The
                    // router internally falls back to the CommandHandler
                    // keyword table when Qwen is unavailable or the JSON
                    // can't parse, so Phase 4 behavior is preserved as a
                    // safety net.
                    auto tier = jarvis::net::WiFiManager::getConnectivityTier();
                    RouteResult result = jarvis::app::route(final_text, tier);
                    enterSpeaking(result.spoken);
                }
            } else if ((int32_t)(millis() - g_listen_deadline_ms) >= 0) {
                Serial.println("[FSM] LISTENING timeout");
                enterIdle();
            }
            break;
        }

        case DeviceState::THINKING: {
            // Transient — entered by enterTranscribing() and immediately
            // exited by enterSpeaking() / enterIdle() in the same tick.
            // Reaching the switch here means something asked us to think
            // without giving us anything to say. Shouldn't happen.
            break;
        }

        case DeviceState::SPEAKING: {
            if (g_speak_done) {
                g_speak_done = false;
                enterIdle();
            }
            break;
        }

        case DeviceState::ERROR: {
            // Phase 2 doesn't generate ERROR transitions; placeholder for
            // Phase 4+. ERR_* TTS plays, then back to IDLE.
            if (g_speak_done) {
                g_speak_done = false;
                enterIdle();
            }
            break;
        }
    }
}

DeviceState currentState() { return g_state; }

}  // namespace jarvis::app

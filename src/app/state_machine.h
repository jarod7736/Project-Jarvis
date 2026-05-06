#pragma once

#include "../hal/Display.h"
#include "../hal/LLMModule.h"

namespace jarvis::app {

// Phase 2 FSM. Drives the voice loop:
//   IDLE → LISTENING → TRANSCRIBING → SPEAKING → IDLE
//   ERROR → IDLE
//
// TRANSCRIBING → SPEAKING is a temporary echo path (replays the transcript
// via TTS) so we can exercise the full audio loop before Phase 5 routing
// exists. It gets replaced wholesale, and is not load-bearing.
//
// All transitions happen in tickStateMachine(); callbacks set flags only.
void stateMachineBegin(jarvis::hal::LLMModule* module);
void tickStateMachine();

jarvis::hal::DeviceState currentState();

}  // namespace jarvis::app

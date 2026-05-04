// Project Jarvis — Phase 1 Hardware Validation
//
// Mirrors the bundled M5Module-LLM VoiceAssistant example, with two additions:
//   (1) USB-CDC Serial logging so `pio device monitor` shows the same lifecycle
//       the on-device display shows (and any ASR/LLM JSON the lib surfaces).
//   (2) module_llm.sys.version() capture after begin() — the StackFlow firmware
//       version we need to pin into config.h (per CLAUDE.md).
//
// Wake word here is "HELLO" (the bundled KWS asset). Custom "JARVIS" KWS is
// Phase 2 work, not Phase 1.
//
// Pinned StackFlow FW (LLM Module) at Phase 1 validation: v1.3 (2026-05-03).
// Re-test the UART JSON path after every StackFlow FW bump — the API drifts.
// Move this constant into config.h when that file is created in Phase 2.

#include <Arduino.h>
#include <M5Unified.h>
#include <M5ModuleLLM.h>

M5ModuleLLM module_llm;
M5ModuleLLM_VoiceAssistant voice_assistant(&module_llm);

void on_asr_data_input(String data, bool isFinish, int index)
{
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.printf(">> %s\n", data.c_str());
    Serial.printf("[ASR] finish=%d idx=%d text=%s\n", isFinish, index, data.c_str());

    if (isFinish) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.print(">> ");
    }
}

// Qwen streams response tokens as fragments. Mirror them to the display live
// so the user sees the reply forming, but accumulate into one buffer and emit
// a single greppable `[LLM] idx=N reply="..."` line on isFinish — the streaming
// fragments have no prefix and were getting lost in serial-log filters.
static String llm_accum;

void on_llm_data_input(String data, bool isFinish, int index)
{
    M5.Display.print(data);
    llm_accum += data;

    if (isFinish) {
        M5.Display.print("\n");
        Serial.printf("[LLM] idx=%d reply=\"%s\"\n", index, llm_accum.c_str());
        llm_accum = "";
    }
}

void setup()
{
    M5.begin();
    M5.Display.setTextSize(2);
    M5.Display.setTextScroll(true);

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("=== Project Jarvis — Phase 1 Hardware Validation ===");

    int rxd = M5.getPin(m5::pin_name_t::port_c_rxd);
    int txd = M5.getPin(m5::pin_name_t::port_c_txd);
    Serial.printf("[UART] Port C pins: RX=%d TX=%d @ 115200 8N1\n", rxd, txd);
    Serial2.begin(115200, SERIAL_8N1, rxd, txd);

    module_llm.begin(&Serial2);

    M5.Display.printf(">> Check ModuleLLM connection..\n");
    Serial.print("[CONN] Waiting for ModuleLLM");
    while (!module_llm.checkConnection()) {
        Serial.print(".");
        delay(500);
    }
    Serial.println(" OK");

    String fw = module_llm.sys.version();
    M5.Display.printf(">> StackFlow FW: %s\n", fw.c_str());
    Serial.printf("[FW] StackFlow version: %s\n", fw.c_str());

    M5.Display.printf(">> Begin voice assistant..\n");
    int ret = voice_assistant.begin("HELLO");
    if (ret != MODULE_LLM_OK) {
        Serial.printf("[ERR] voice_assistant.begin() returned %d\n", ret);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.printf(">> Begin voice assistant failed (%d)\n", ret);
        while (1) {
            delay(1000);
        }
    }

    voice_assistant.onAsrDataInput(on_asr_data_input);
    voice_assistant.onLlmDataInput(on_llm_data_input);

    M5.Display.printf(">> Voice assistant ready\n");
    Serial.println("[READY] Say \"HELLO\" to wake.");
}

void loop()
{
    voice_assistant.update();
}

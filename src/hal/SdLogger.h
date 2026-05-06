#pragma once

// SdLogger — Phase 7 logging: append a JSON line per voice exchange to the
// SD card. Useful for offline review, regression testing, and tuning the
// intent router against a corpus of real queries.
//
// Hardware: CoreS3's SD slot shares SPI with the display (CLAUDE.md
// SPI-conflict rule). We initialise SD on its OWN SPIClass instance with
// explicit pin assignment so the display's bus state never bleeds into the
// SD path.
//
// Failure mode: missing/unformatted SD card is non-fatal. begin() returns
// false, logs once at boot, and every subsequent logExchange() is a no-op.
// Voice loop stays functional.

#include <Arduino.h>

namespace jarvis::hal {

class SdLogger {
public:
    // Mount the SD card on its own SPIClass. Returns true on success.
    // Idempotent — safe to call again, but a second call after a failed
    // first attempt won't retry the mount (just returns the cached state).
    static bool begin();

    // True iff begin() succeeded and the card is still mounted.
    static bool ok();

    // Append one JSON line summarising a voice exchange. No-op when the
    // card isn't mounted. Cheap on success — single fopen/append/fclose;
    // SD library buffers internally. Don't call from interrupts.
    //   tier_label: "LAN"/"TS"/"HOT"/"OFF" — comes free from
    //               jarvis::net::tierName() at the call site.
    //   latency_ms: wall-clock from receiving final transcript to having
    //               a reply ready. Captures network/LLM time.
    static void logExchange(const String& transcript,
                            const String& response,
                            const char*   tier_label,
                            uint32_t      latency_ms);
};

}  // namespace jarvis::hal

#include "SdLogger.h"

#include <SD.h>
#include <SPI.h>

#include "../config.h"

namespace jarvis::hal {

namespace {

// Use the GLOBAL `SPI` instance, not a fresh SPIClass(HSPI). On ESP32-S3
// HSPI maps to the same SPI host that M5GFX uses for the CoreS3 display,
// so allocating a new bus there freezes the screen the moment SD.begin()
// runs. M5Stack's own examples/Basic/sdcard/sdcard.ino uses `SPI` and
// lets the Arduino SPI library arbitrate between SD and display via
// beginTransaction(). Same pins (35/36/37/4), one shared host, no
// freeze.
bool g_ok = false;
bool g_attempted = false;

constexpr const char* kLogPath = "/jarvis.log";

// Escape a String for JSON. Quotes and backslashes are the realistic
// problem cases; control chars get a generic \uXXXX. Keep it small —
// transcripts and responses are short on this device.
String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

bool SdLogger::begin() {
    if (g_attempted) return g_ok;
    g_attempted = true;

    using namespace jarvis::config;
    // Share the global SPI host with M5GFX. Beginning here re-pins the
    // bus to SD's pin set; the bus stays alive but Arduino SPI handles
    // per-transaction reconfiguration so the display still works.
    SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);

    // 25 MHz is the safe default for CoreS3's SD slot; 40 MHz works on
    // some cards but isn't worth the marginal speed for our line-at-a-
    // time workload.
    if (!SD.begin(kSdCs, SPI, 25000000)) {
        Serial.println("[SdLogger] SD.begin failed (no card / unformatted?) — logging disabled");
        g_ok = false;
        return false;
    }

    uint8_t type = SD.cardType();
    if (type == CARD_NONE) {
        Serial.println("[SdLogger] SD reports CARD_NONE — logging disabled");
        g_ok = false;
        return false;
    }
    uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("[SdLogger] mounted: type=%u size=%lluMB path=%s\n",
                  (unsigned)type, (unsigned long long)mb, kLogPath);

    g_ok = true;
    return true;
}

bool SdLogger::ok() { return g_ok; }

void SdLogger::logExchange(const String& transcript, const String& response,
                           const char* tier_label, uint32_t latency_ms) {
    if (!g_ok) return;

    File f = SD.open(kLogPath, FILE_APPEND);
    if (!f) {
        // Card might've been pulled. Mark dead so we stop retrying — a
        // re-insert without reboot won't be picked up either way (no
        // hot-plug detect on this slot).
        Serial.println("[SdLogger] open(APPEND) failed — disabling");
        g_ok = false;
        return;
    }

    // One JSON object per line. ts is uptime ms since boot; getLocalTime
    // would be nicer but NTP isn't guaranteed up at log time and we'd
    // rather have a monotonic timestamp than a maybe-zero wall clock.
    String line;
    line.reserve(64 + transcript.length() + response.length());
    line += "{\"ts\":";
    line += millis();
    line += ",\"latency_ms\":";
    line += latency_ms;
    line += ",\"tier\":\"";
    line += tier_label ? tier_label : "?";
    line += "\",\"transcript\":\"";
    line += jsonEscape(transcript);
    line += "\",\"response\":\"";
    line += jsonEscape(response);
    line += "\"}\n";

    size_t written = f.print(line);
    f.close();
    if (written != line.length()) {
        Serial.printf("[SdLogger] short write %u/%u — card full?\n",
                      (unsigned)written, (unsigned)line.length());
    }
}

}  // namespace jarvis::hal

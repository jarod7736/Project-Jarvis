#pragma once

// OtaService — Phase 7 over-the-air firmware updates.
//
// Two paths:
//  - LAN push:  ArduinoOTA listens for IDE/PlatformIO uploads at port 3232,
//               authenticated with the password stored in NVS as `ota_pass`.
//               If `ota_pass` is empty, the service stays disabled (safe
//               default per CLAUDE.md credentials-in-NVS rule).
//  - Remote pull: HTTPUpdate downloads a .bin from the URL in NVS `fw_url`,
//               triggered by the "update firmware" voice intent. Blocking;
//               on success the device reboots inside update().
//
// Both paths feed the loop watchdog from a progress callback so a multi-
// second flash write doesn't trip the 30 s timeout in main.cpp.

#include <Arduino.h>

namespace jarvis::net {

class OtaService {
public:
    static bool begin();                        // wire ArduinoOTA; safe to skip if disabled
    static void tick();                         // pump ArduinoOTA.handle()
    static bool isActive();                     // true mid-LAN-flash or mid-HTTPUpdate
    static bool pullRemote(const String& url);  // HTTPUpdate over WiFiClient[Secure]
};

}  // namespace jarvis::net

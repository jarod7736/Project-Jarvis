// Jarvis - Voice AI Edge Device
// Target: M5Stack CoreS3 (ESP32-S3) + LLM Module
//
// Two operating modes, switched by long-press on the touchscreen:
//   NORMAL MODE -- WiFi STA, voice pipeline running, no web server.
//   CONFIG MODE -- WiFi AP (Jarvis-Setup), captive portal at 192.168.4.1.
//
// Web UI is served from LittleFS (./data/web/ in the source tree).
// Flash with:  pio run --target uploadfs

#include <M5Unified.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

// ============================================================================
// SECTION 1: Config Schema -- single source of truth
// ============================================================================

enum class FieldType : uint8_t { Bool, Int, String, Enum };

struct EnumOption { const char* value; const char* label; };

struct ConfigField {
  const char* key;        // NVS key (max 15 chars), also JSON key
  const char* label;
  const char* category;
  FieldType   type;
  bool        sensitive;  // Mask in UI, never log
  bool        on_device;  // Show on the 2" touchscreen?

  struct {
    int min;
    int max;
    int default_int;
    bool default_bool;
    const char* default_string;
    const EnumOption* enum_options;
    size_t enum_count;
  } meta;
};

static constexpr EnumOption kTierOptions[] = {
  {"auto",  "Auto (route via Qwen)"},
  {"local", "Local LLM only"},
  {"cloud", "Force Claude"},
  {"qwen",  "Qwen only (offline)"},
};

static const ConfigField kSchema[] = {
  // --- Audio ---
  { "tts_volume",   "TTS Volume",         "audio",   FieldType::Int,    false, true,
    {.min=0, .max=100, .default_int=70} },
  { "wake_sens",    "Wake Sensitivity",   "audio",   FieldType::Int,    false, true,
    {.min=1, .max=10, .default_int=5} },
  { "mic_gain",     "Mic Gain",           "audio",   FieldType::Int,    false, true,
    {.min=0, .max=100, .default_int=50} },

  // --- Routing ---
  { "default_tier", "Default Tier",       "routing", FieldType::Enum,   false, true,
    {.enum_options=kTierOptions, .enum_count=4} },
  { "route_timeout","Route Timeout (ms)", "routing", FieldType::Int,    false, false,
    {.min=500, .max=10000, .default_int=3000} },
  { "log_to_sd",    "Log to SD card",     "routing", FieldType::Bool,   false, true,
    {.default_bool=true} },

  // --- Endpoints ---
  { "openclaw_url", "OpenClaw URL",       "network", FieldType::String, false, false,
    {.default_string="https://lobsterboy.tail1c66ec.ts.net"} },
  { "ha_url",       "Home Assistant URL", "network", FieldType::String, false, false,
    {.default_string="http://homeassistant.local:8123"} },
  { "ha_token",     "HA Long-Lived Token","network", FieldType::String, true,  false,
    {.default_string=""} },

  // --- Display ---
  { "brightness",   "Display Brightness", "display", FieldType::Int,    false, true,
    {.min=10, .max=255, .default_int=180} },
  { "sleep_secs",   "Sleep After (sec)",  "display", FieldType::Int,    false, true,
    {.min=0, .max=600, .default_int=60} },
};
static constexpr size_t kSchemaCount = sizeof(kSchema) / sizeof(kSchema[0]);

// ============================================================================
// SECTION 2: Config storage (NVS bridge)
// ============================================================================

class ConfigStore {
 public:
  void begin() { prefs_.begin("jarvis", false); }

  int    getInt   (const char* k, int def)            { return prefs_.getInt(k, def); }
  bool   getBool  (const char* k, bool def)           { return prefs_.getBool(k, def); }
  String getString(const char* k, const char* def="") { return prefs_.getString(k, def); }

  void setInt   (const char* k, int v)            { prefs_.putInt(k, v); }
  void setBool  (const char* k, bool v)           { prefs_.putBool(k, v); }
  void setString(const char* k, const String& v)  { prefs_.putString(k, v); }

  int    fieldInt(const ConfigField& f)    { return getInt(f.key, f.meta.default_int); }
  bool   fieldBool(const ConfigField& f)   { return getBool(f.key, f.meta.default_bool); }
  String fieldString(const ConfigField& f) {
    return getString(f.key, f.meta.default_string ? f.meta.default_string : "");
  }

 private:
  Preferences prefs_;
};

ConfigStore g_config;

// ============================================================================
// SECTION 3: Schema <-> JSON
// ============================================================================

void buildConfigJson(JsonDocument& doc) {
  JsonArray fields = doc["fields"].to<JsonArray>();
  for (size_t i = 0; i < kSchemaCount; ++i) {
    const auto& f = kSchema[i];
    JsonObject o = fields.add<JsonObject>();
    o["key"]       = f.key;
    o["label"]     = f.label;
    o["category"]  = f.category;
    o["sensitive"] = f.sensitive;

    switch (f.type) {
      case FieldType::Bool:
        o["type"]    = "bool";
        o["value"]   = g_config.fieldBool(f);
        o["default"] = f.meta.default_bool;
        break;
      case FieldType::Int:
        o["type"]    = "int";
        o["value"]   = g_config.fieldInt(f);
        o["min"]     = f.meta.min;
        o["max"]     = f.meta.max;
        o["default"] = f.meta.default_int;
        break;
      case FieldType::String:
        o["type"]  = "string";
        o["value"] = f.sensitive
            ? (g_config.fieldString(f).length() ? "********" : "")
            : g_config.fieldString(f);
        break;
      case FieldType::Enum: {
        o["type"]  = "enum";
        o["value"] = g_config.fieldString(f);
        JsonArray opts = o["options"].to<JsonArray>();
        for (size_t j = 0; j < f.meta.enum_count; ++j) {
          JsonObject opt = opts.add<JsonObject>();
          opt["value"] = f.meta.enum_options[j].value;
          opt["label"] = f.meta.enum_options[j].label;
        }
        break;
      }
    }
  }
}

int applyConfigJson(const JsonDocument& patch) {
  int updated = 0;
  for (size_t i = 0; i < kSchemaCount; ++i) {
    const auto& f = kSchema[i];
    if (!patch[f.key].is<JsonVariantConst>()) continue;

    switch (f.type) {
      case FieldType::Bool:
        g_config.setBool(f.key, patch[f.key].as<bool>());
        break;
      case FieldType::Int: {
        int v = patch[f.key].as<int>();
        if (v < f.meta.min || v > f.meta.max) return -1;
        g_config.setInt(f.key, v);
        break;
      }
      case FieldType::String: {
        String v = patch[f.key].as<String>();
        if (f.sensitive && v == "********") continue;
        g_config.setString(f.key, v);
        break;
      }
      case FieldType::Enum: {
        String v = patch[f.key].as<String>();
        bool valid = false;
        for (size_t j = 0; j < f.meta.enum_count; ++j) {
          if (v == f.meta.enum_options[j].value) { valid = true; break; }
        }
        if (!valid) return -1;
        g_config.setString(f.key, v);
        break;
      }
    }
    ++updated;
  }
  return updated;
}

// ============================================================================
// SECTION 4: WiFi credentials (separate NVS namespace from config)
// ============================================================================

namespace wifi_creds {
  constexpr size_t kMaxNetworks = 5;
  struct Network { String ssid; String password; };

  void load(Network* out, size_t& count) {
    Preferences p;
    p.begin("wifi", true);
    count = p.getUChar("n", 0);
    if (count > kMaxNetworks) count = kMaxNetworks;
    for (size_t i = 0; i < count; ++i) {
      char k[8];
      snprintf(k, sizeof(k), "s%zu", i); out[i].ssid     = p.getString(k, "");
      snprintf(k, sizeof(k), "p%zu", i); out[i].password = p.getString(k, "");
    }
    p.end();
  }

  void add(const String& ssid, const String& password) {
    Network nets[kMaxNetworks]; size_t n = 0;
    load(nets, n);
    for (size_t i = 0; i < n; ++i) {
      if (nets[i].ssid == ssid) { nets[i].password = password; goto save; }
    }
    if (n < kMaxNetworks) ++n;
    for (size_t i = n - 1; i > 0; --i) nets[i] = nets[i - 1];
    nets[0] = {ssid, password};
   save:
    Preferences p;
    p.begin("wifi", false);
    p.putUChar("n", n);
    for (size_t i = 0; i < n; ++i) {
      char k[8];
      snprintf(k, sizeof(k), "s%zu", i); p.putString(k, nets[i].ssid);
      snprintf(k, sizeof(k), "p%zu", i); p.putString(k, nets[i].password);
    }
    p.end();
  }

  void list(JsonArray& arr) {
    Network nets[kMaxNetworks]; size_t n = 0;
    load(nets, n);
    for (size_t i = 0; i < n; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"]     = nets[i].ssid;
      o["priority"] = (int)i;
    }
  }

  void remove(const String& ssid) {
    Network nets[kMaxNetworks]; size_t n = 0;
    load(nets, n);
    size_t out = 0;
    for (size_t i = 0; i < n; ++i) {
      if (nets[i].ssid != ssid) {
        if (out != i) nets[out] = nets[i];
        ++out;
      }
    }
    Preferences p;
    p.begin("wifi", false);
    p.putUChar("n", out);
    for (size_t i = 0; i < out; ++i) {
      char k[8];
      snprintf(k, sizeof(k), "s%zu", i); p.putString(k, nets[i].ssid);
      snprintf(k, sizeof(k), "p%zu", i); p.putString(k, nets[i].password);
    }
    p.end();
  }
}

// ============================================================================
// SECTION 5: Mode manager
// ============================================================================

AsyncWebServer g_server(80);
DNSServer      g_dns;
volatile bool  g_exit_requested = false;
bool           g_portal_running = false;

class ModeManager {
 public:
  enum class Mode : uint8_t { Booting, Normal, Config };

  void begin() { enterNormal(); }
  Mode mode() const { return mode_; }
  bool isConfig() const { return mode_ == Mode::Config; }

  void enterNormal() {
    teardownPortal();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("jarvis");
    connectKnownNetworks();
    mode_ = Mode::Normal;
    last_activity_ms_ = 0;
    Serial.println("[MODE] Normal");
  }

  void enterConfig() {
    if (mode_ == Mode::Config) return;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Jarvis-Setup", nullptr);
    setupPortal();
    mode_ = Mode::Config;
    last_activity_ms_ = millis();
    Serial.print("[MODE] Config -- AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  void noteActivity() { last_activity_ms_ = millis(); }

  void tickInactivity() {
    if (mode_ != Mode::Config) return;
    if (last_activity_ms_ == 0) return;
    if (millis() - last_activity_ms_ > 5 * 60 * 1000UL) enterNormal();
  }

 private:
  Mode     mode_ = Mode::Booting;
  uint32_t last_activity_ms_ = 0;

  void connectKnownNetworks() {
    wifi_creds::Network nets[wifi_creds::kMaxNetworks];
    size_t n = 0;
    wifi_creds::load(nets, n);
    if (n == 0) {
      Serial.println("[WIFI] No saved networks");
      return;
    }
    Serial.printf("[WIFI] Connecting to %s\n", nets[0].ssid.c_str());
    WiFi.begin(nets[0].ssid.c_str(), nets[0].password.c_str());
  }

  void setupPortal();
  void teardownPortal() {
    if (!g_portal_running) return;
    g_dns.stop();
    g_server.end();
    g_portal_running = false;
  }
};

ModeManager g_mode;

// ============================================================================
// SECTION 6: Captive portal (only in config mode)
// ============================================================================

// Helper: read a JSON body from request data and return parsed doc + status.
// AsyncWebServer fragments POST bodies; we accumulate manually.
struct PendingBody {
  String data;
  size_t expected = 0;
};
static PendingBody g_pending_config_body;
static PendingBody g_pending_wifi_body;

void ModeManager::setupPortal() {
  if (g_portal_running) return;

  // ---- Static UI from LittleFS ----
  // The ./data/web/ folder gets packed into the LittleFS partition.
  // serveStatic() handles index.html, CSS, JS automatically.
  g_server.serveStatic("/", LittleFS, "/web/")
          .setDefaultFile("index.html");

  // ---- API routes ----

  // GET /api/config -> schema + current values
  g_server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    g_mode.noteActivity();
    JsonDocument doc;
    buildConfigJson(doc);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /api/config -> patch values
  g_server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total) {
      g_mode.noteActivity();
      if (idx == 0) g_pending_config_body.data = "";
      g_pending_config_body.data.concat((const char*)data, len);
      if (idx + len < total) return;  // wait for more chunks

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, g_pending_config_body.data);
      g_pending_config_body.data = "";  // free
      if (err) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
      }
      int n = applyConfigJson(doc);
      if (n < 0) {
        req->send(400, "application/json", "{\"error\":\"validation failed\"}");
        return;
      }
      JsonDocument resp;
      resp["updated"] = n;
      String out;
      serializeJson(resp, out);
      req->send(200, "application/json", out);
    });

  // GET /api/wifi/scan -> trigger or read async scan
  g_server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    g_mode.noteActivity();
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      req->send(202, "application/json", "{\"status\":\"scanning\"}");
      return;
    }
    if (n < 0) {
      WiFi.scanNetworks(true);
      req->send(202, "application/json", "{\"status\":\"started\"}");
      return;
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"]   = WiFi.SSID(i);
      o["rssi"]   = WiFi.RSSI(i);
      o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    String out;
    serializeJson(doc, out);
    WiFi.scanDelete();
    req->send(200, "application/json", out);
  });

  // GET /api/wifi/saved -> list of saved networks
  g_server.on("/api/wifi/saved", HTTP_GET, [](AsyncWebServerRequest* req) {
    g_mode.noteActivity();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    wifi_creds::list(arr);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /api/wifi/add { ssid, password }
  g_server.on("/api/wifi/add", HTTP_POST,
    [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total) {
      g_mode.noteActivity();
      if (idx == 0) g_pending_wifi_body.data = "";
      g_pending_wifi_body.data.concat((const char*)data, len);
      if (idx + len < total) return;

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, g_pending_wifi_body.data);
      g_pending_wifi_body.data = "";
      if (err) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
      }
      String ssid = doc["ssid"].as<String>();
      String pass = doc["password"].as<String>();
      if (ssid.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
      }
      wifi_creds::add(ssid, pass);
      req->send(200, "application/json", "{\"saved\":true}");
    });

  // POST /api/wifi/remove?ssid=...
  g_server.on("/api/wifi/remove", HTTP_POST, [](AsyncWebServerRequest* req) {
    g_mode.noteActivity();
    if (!req->hasParam("ssid")) {
      req->send(400, "application/json", "{\"error\":\"ssid required\"}");
      return;
    }
    wifi_creds::remove(req->getParam("ssid")->value());
    req->send(200, "application/json", "{\"removed\":true}");
  });

  // GET /api/status -> live runtime info for the status tab
  g_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    g_mode.noteActivity();
    JsonDocument doc;
    doc["battery_pct"]  = M5.Power.getBatteryLevel();
    doc["charging"]     = M5.Power.isCharging();
    doc["uptime_ms"]    = millis();
    doc["free_heap"]    = ESP.getFreeHeap();
    doc["mode"]         = "config";
    doc["ap_ssid"]      = "Jarvis-Setup";
    doc["ap_clients"]   = WiFi.softAPgetStationNum();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /api/exit -- return to normal mode
  g_server.on("/api/exit", HTTP_POST, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"exiting\":true}");
    g_exit_requested = true;
  });

  // Captive portal: catch-all redirect.
  // iOS hits /hotspot-detect.html, Android hits /generate_204, Windows hits
  // /connecttest.txt -- redirecting them all triggers the OS captive portal UI.
  g_server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("http://192.168.4.1/");
  });

  g_server.begin();
  g_dns.start(53, "*", IPAddress(192, 168, 4, 1));
  g_portal_running = true;
}

// ============================================================================
// SECTION 7: On-device display (M5GFX status -- LVGL deferred for now)
// ============================================================================
// Using M5GFX directly here for a minimal status display. Full LVGL screens
// can layer in later; for the LittleFS-hosting milestone we just need to
// show enough info that the device is usable while in config mode.

void drawHomeScreen() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(WHITE);

  // Status bar
  M5.Display.setTextSize(1);
  M5.Display.setCursor(4, 4);
  M5.Display.printf("BAT %d%%%s   ",
                    M5.Power.getBatteryLevel(),
                    M5.Power.isCharging() ? "+" : "");
  if (WiFi.isConnected()) {
    M5.Display.printf("WIFI %ddBm", WiFi.RSSI());
  } else {
    M5.Display.print("WIFI --");
  }

  // Body
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 60);
  M5.Display.println("JARVIS");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 100);
  M5.Display.println("Listening...");

  M5.Display.setCursor(8, 220);
  M5.Display.setTextColor(0x7BEF);  // dim gray
  M5.Display.println("Hold screen 2s for Config");
}

void drawConfigScreen() {
  M5.Display.fillScreen(0x0841);  // very dark blue
  M5.Display.setTextDatum(top_left);

  M5.Display.setTextColor(0x07E0);  // green
  M5.Display.setTextSize(2);
  M5.Display.setCursor(60, 20);
  M5.Display.println("CONFIG MODE");

  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(20, 70);
  M5.Display.println("Connect phone to WiFi:");

  M5.Display.setTextSize(2);
  M5.Display.setCursor(40, 100);
  M5.Display.setTextColor(0x07E0);
  M5.Display.println("Jarvis-Setup");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(20, 150);
  M5.Display.println("Then open in browser:");
  M5.Display.setCursor(40, 170);
  M5.Display.setTextColor(0x07E0);
  M5.Display.print("http://192.168.4.1");

  M5.Display.setTextColor(0x7BEF);
  M5.Display.setCursor(40, 220);
  M5.Display.println("Hold screen 2s to exit");
}

void updateStatusBar() {
  // Just redraw the top strip without clearing the rest
  M5.Display.fillRect(0, 0, 320, 16, BLACK);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(4, 4);
  M5.Display.printf("BAT %d%%%s   ",
                    M5.Power.getBatteryLevel(),
                    M5.Power.isCharging() ? "+" : "");
  if (g_mode.isConfig()) {
    M5.Display.printf("AP  %d client(s)", WiFi.softAPgetStationNum());
  } else if (WiFi.isConnected()) {
    M5.Display.printf("WIFI %ddBm", WiFi.RSSI());
  } else {
    M5.Display.print("WIFI --");
  }
}

// ============================================================================
// SECTION 8: Long-press detector
// ============================================================================

class LongPressDetector {
 public:
  bool tick() {
    bool touched = M5.Touch.getCount() > 0;
    uint32_t now = millis();

    if (touched && !was_touched_) {
      press_start_ = now;
      fired_       = false;
    }
    if (touched && !fired_ && (now - press_start_) >= kHoldMs) {
      fired_ = true;
      was_touched_ = touched;
      return true;
    }
    was_touched_ = touched;
    return false;
  }

 private:
  static constexpr uint32_t kHoldMs = 2000;
  bool     was_touched_ = false;
  bool     fired_ = false;
  uint32_t press_start_ = 0;
};

LongPressDetector g_press;

// ============================================================================
// SECTION 9: Wiring
// ============================================================================

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] Jarvis starting");

  g_config.begin();
  M5.Display.setBrightness(g_config.fieldInt(kSchema[9]));  // brightness

  // LittleFS mount -- contains web UI files
  if (!LittleFS.begin(true)) {  // true = format on failure
    Serial.println("[FS] LittleFS mount FAILED");
  } else {
    Serial.printf("[FS] LittleFS OK  used=%u/%u\n",
                  LittleFS.usedBytes(), LittleFS.totalBytes());
  }

  drawHomeScreen();
  g_mode.begin();
}

void loop() {
  M5.update();

  // Mode toggle via long-press
  if (g_press.tick()) {
    if (g_mode.isConfig()) {
      g_mode.enterNormal();
      drawHomeScreen();
    } else {
      g_mode.enterConfig();
      drawConfigScreen();
    }
  }

  if (g_exit_requested) {
    g_exit_requested = false;
    g_mode.enterNormal();
    drawHomeScreen();
  }

  g_mode.tickInactivity();

  // Periodic status refresh (only the top strip; body is event-driven)
  static uint32_t last_status = 0;
  if (millis() - last_status > 2000) {
    updateStatusBar();
    last_status = millis();
  }

  if (g_mode.isConfig()) g_dns.processNextRequest();

  delay(5);
}

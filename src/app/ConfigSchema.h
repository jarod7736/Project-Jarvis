#pragma once

// ConfigSchema — single source of truth for the captive-portal /api/config
// endpoint. The schema table drives both:
//   - The JSON response body (each row becomes a `fields[].{key,label,type,
//     value,...}` object) consumed by data/web/app.js to render form
//     controls.
//   - The validation+write path on POST: each present key in the patch is
//     range-checked, then forwarded to NVS via the matching NVSConfig
//     setter.
//
// Schema fields map directly to NVS keys (≤15 chars per CLAUDE.md). Some
// fields have a dedicated typed accessor in NVSConfig (e.g. `getHaToken`),
// others are written through the generic Preferences("jarvis") namespace
// when no dedicated accessor exists.
//
// To add a new field:
//   1. Pick an NVS key (≤15 chars) and add a typed accessor pair in
//      NVSConfig if its semantics warrant one (range validation, default
//      fallback). Otherwise leave it as a generic Preferences entry.
//   2. Append a row to kSchema in ConfigSchema.cpp.
//   3. The web UI re-renders automatically — schema-driven.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace jarvis::app {

// Build the full schema+values JSON document into `doc`. Shape:
//   {
//     "fields": [
//       {"key":"ha_token","label":"HA Token","category":"network",
//        "type":"string","sensitive":true,"value":"********"},
//       ...
//     ]
//   }
// Sensitive string values are masked: stored value present → "********",
// stored value empty → "". The web UI relies on this convention to know
// when to show the unchanged-placeholder hint.
void buildConfigJson(JsonDocument& doc);

// Apply a patch document. Each present key in `patch` whose value passes
// per-field validation is written through to NVS. Returns the number of
// fields applied, or -1 on the first hard validation failure (range, enum
// membership). Sensitive strings equal to "********" are skipped (the user
// kept the masked placeholder → no change intended).
int applyConfigJson(const JsonDocument& patch);

}  // namespace jarvis::app

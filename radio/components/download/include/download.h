// Download mode: the boot that downloads a new frequency guide table (Settings > Guide > Update guide) or new firmware
// (Settings > Firmware > Update), with only the display, the knob and WiFi started.
#pragma once
#include <cstddef>
#include <cstdint>

#include "download_core.h"

namespace download {

// At boot, once NVS is up: forgets last_update at a power-on reset, and takes the stored request (cleared first, so a
// crash during the download cannot repeat it).
Request takeRequest();
// Runs the download, stores the result and restarts. `version` is the running firmware's.
[[noreturn]] void run(int lang, Request r, const char* version);
// Stores a request for the next boot (the menu; the caller restarts).
void request(Request r);
// POST /api/guide/update and /api/firmware/update: 409 without a configured network, else the request stored and 202.
int requestUpdate(Request r, bool netSaved, char* body, size_t n);
// The last download since power-up (last_update in GET /api/guide and GET /api/firmware).
bool lastUpdate(LastUpdate& out);
// The text home shows once for the result the last download stored, or nullptr for none; the result is cleared.
const char* takeResultText(int lang);

}  // namespace download

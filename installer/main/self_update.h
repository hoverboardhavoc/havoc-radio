// The installer's own firmware update (installer/SPEC.md, "Updating the
// installer itself"). POST /api/installer/update writes the app image in the
// body to the OTA slot the installer does not run from, with ESP-IDF's OTA,
// and restarts into it. A new firmware starts pending verification and marks
// itself valid once the HTTP server has answered on the hotspot address for
// 30 s; a restart before that makes the bootloader return to the previous slot.

#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_http_server.h"

void self_update_http_register(httpd_handle_t server);
// Log the running firmware and, while it is pending verification, start the
// check that marks it valid. Call once the HTTP server runs.
void self_update_start(void);
// An update is being written, or the installer is about to restart into one.
bool self_update_running(void);
// {"version", "built", "slot", "confirmed", "update": null | {"written", "total"}}
cJSON *self_update_json(void);

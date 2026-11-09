// The carried image in the "stage" partition (installer/SPEC.md, "The carried
// image"): the header in sector 0, the data regions after it, the /stage HTTP
// endpoints that replace it, and the reads of the WiFi install and the flash
// job. Layout and bookkeeping live in stage_layout.c.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "stage_layout.h"

// Read the header and check every data region's SHA-256, so a freshly flashed
// C3 serves its image without an upload.
esp_err_t stage_init(void);
void stage_http_register(httpd_handle_t server);

// The image is complete: every data region present and checked (by the boot
// check or the last /stage/check, with nothing changed since).
bool stage_complete(void);
// The current manifest. Stable while a flash job runs or a WiFi install holds
// the image: the stage endpoints refuse changes then.
const sl_manifest_t *stage_manifest(void);
esp_err_t stage_read(uint32_t addr, void *buf, size_t len);
// MD5 (32 hex digits) of what region r should hold on the target: its staged
// bytes, or 0xFF for an erase-only region.
esp_err_t stage_region_md5(const sl_region_t *r, char hex[33]);

// For a WiFi install session: NULL when the image is complete, else why the
// session cannot have it (no complete image, or an installer update runs); len
// gets the flat length. The image stays unchanged until stage_release().
const char *stage_hold(uint32_t *len);
void stage_release(void);
// A WiFi install session holds the image.
bool stage_held(void);
// len bytes of the flat image at offset: data regions from the stage, the rest 0xFF.
bool stage_flat_read(uint32_t offset, uint8_t *buf, size_t len);

// The carried image for /api/status.
cJSON *stage_status_json(void);

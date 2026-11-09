// The flash job: writes the carried image's regions to the radio over its
// serial header with esp-serial-flasher, and the /flash HTTP endpoints.

#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_http_server.h"

bool flash_job_running(void);
// The job state, as GET /flash answers it.
cJSON *flash_job_json(void);
void flash_job_http_register(httpd_handle_t server);

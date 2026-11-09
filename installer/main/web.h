// The HTTP server on port 80 of both interfaces: the page, the captive-portal
// redirect, /api/status, /api/wifi and /status. The stage, flash-job, serial and
// self-update endpoints register themselves on the same server.

#pragma once

#include "esp_http_server.h"

httpd_handle_t web_start(void);

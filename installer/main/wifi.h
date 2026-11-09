// The hotspot "Havoc" and the optional home network (installer/SPEC.md,
// "WiFi"), with mDNS on both.

#pragma once

#include "cJSON.h"
#include "esp_err.h"

#define WIFI_HOTSPOT_IP_TEXT "192.168.4.1"

// Start the hotspot, and the home network when one is stored in NVS.
void wifi_start(void);
// Store and join a home network; an empty ssid forgets it.
esp_err_t wifi_set_home(const char *ssid, const char *password);
// {"ssid", "state": "off"|"joining"|"joined", "ip"}
cJSON *wifi_home_json(void);

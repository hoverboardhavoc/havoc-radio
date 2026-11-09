// The device and download records (records.c) on the C3: fed by the WiFi
// events and the DNS and update servers, read by /api/status.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#include "update_session.h"

void activity_init(void);
void activity_join(const uint8_t mac[6]);
void activity_leave(const uint8_t mac[6]);
void activity_ip(const uint8_t mac[6], const uint8_t ip[4]);
void activity_dns(const uint8_t ip[4], const char *name);
// A WiFi install session started, made progress, or ended (done).
void activity_session(const uint8_t ip[4], const update_session *us, bool done);
bool activity_session_open(void);
// Add "devices" and "downloads" to root, times in ms of the same clock as "now".
void activity_json(cJSON *root);

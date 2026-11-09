#include "activity.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "led.h"
#include "records.h"
#include "server.h"

static records s_rec;
static SemaphoreHandle_t s_lock;
static uint32_t s_session;  // download id of the open session, 0 when none

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

void activity_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    rec_init(&s_rec);
}

void activity_join(const uint8_t mac[6])
{
    LOCK();
    rec_join(&s_rec, mac, platform_now_ms());
    UNLOCK();
}

void activity_leave(const uint8_t mac[6])
{
    LOCK();
    rec_leave(&s_rec, mac, platform_now_ms());
    UNLOCK();
}

void activity_ip(const uint8_t mac[6], const uint8_t ip[4])
{
    LOCK();
    rec_ip(&s_rec, mac, ip);
    UNLOCK();
}

void activity_dns(const uint8_t ip[4], const char *name)
{
    LOCK();
    rec_dns(&s_rec, ip, name);
    UNLOCK();
}

static rec_outcome outcome(update_outcome o)
{
    switch (o) {
    case US_OK: return DL_OK;
    case US_REFUSED: return DL_REFUSED;
    default: return DL_STOPPED;
    }
}

void activity_session(const uint8_t ip[4], const update_session *us, bool done)
{
    uint32_t now = platform_now_ms();
    LOCK();
    if (s_session == 0) {
        s_session = rec_dl_start(&s_rec, ip, us->image_ok ? us->image_len : 0, now);
    }
    rec_dl_update(&s_rec, s_session, us->mac, us->sent);
    if (done) {
        rec_dl_end(&s_rec, s_session, outcome(us->outcome), now);
        s_session = 0;
    }
    UNLOCK();
    if (done && us->outcome == US_OK) {
        led_completed();
    }
}

bool activity_session_open(void) { return s_session != 0; }

static void add_ip(cJSON *o, const char *key, const uint8_t ip[4])
{
    char text[16] = "";
    if (ip[0] || ip[1] || ip[2] || ip[3]) {
        snprintf(text, sizeof(text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    cJSON_AddStringToObject(o, key, text);
}

void activity_json(cJSON *root)
{
    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    cJSON *downloads = cJSON_AddArrayToObject(root, "downloads");
    LOCK();
    rec_expire(&s_rec, platform_now_ms());
    for (int i = 0; i < REC_DEVICES; i++) {
        const rec_device *d = &s_rec.dev[i];
        if (!d->used) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        char mac[REC_MAC_TEXT];
        rec_mac_text(d->mac, mac);
        cJSON_AddStringToObject(o, "mac", mac);
        add_ip(o, "ip", d->ip);
        cJSON_AddNumberToObject(o, "joined", d->joined_ms);
        if (d->present) {
            cJSON_AddNullToObject(o, "left");
        } else {
            cJSON_AddNumberToObject(o, "left", d->left_ms);
        }
        cJSON_AddBoolToObject(o, "stock", d->stock);
        cJSON *names = cJSON_AddArrayToObject(o, "names");
        for (int k = 0; k < d->name_count; k++) {
            cJSON_AddItemToArray(names, cJSON_CreateString(d->names[k]));
        }
        cJSON_AddItemToArray(devices, o);
    }
    // Newest first.
    for (uint32_t id = s_rec.next_id - 1; id > 0; id--) {
        const rec_download *d = NULL;
        for (int i = 0; i < REC_DOWNLOADS && d == NULL; i++) {
            if (s_rec.dl[i].id == id) {
                d = &s_rec.dl[i];
            }
        }
        if (d == NULL) {
            break;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "start", d->start_ms);
        add_ip(o, "ip", d->ip);
        cJSON_AddStringToObject(o, "mac", d->mac);
        cJSON_AddNumberToObject(o, "sent", d->sent);
        cJSON_AddNumberToObject(o, "total", d->total);
        cJSON_AddStringToObject(o, "outcome", rec_outcome_name(d->outcome));
        cJSON_AddItemToArray(downloads, o);
    }
    UNLOCK();
}

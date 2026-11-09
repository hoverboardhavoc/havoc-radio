#include "records.h"

#include <stdio.h>
#include <string.h>

void rec_init(records *r)
{
    memset(r, 0, sizeof(*r));
    r->next_id = 1;
}

static rec_device *find_mac(records *r, const uint8_t mac[6])
{
    for (int i = 0; i < REC_DEVICES; i++) {
        if (r->dev[i].used && memcmp(r->dev[i].mac, mac, 6) == 0) {
            return &r->dev[i];
        }
    }
    return NULL;
}

// A free slot, else the one seen longest ago, preferring stations that left.
static rec_device *slot(records *r)
{
    rec_device *best = NULL;
    for (int i = 0; i < REC_DEVICES; i++) {
        rec_device *d = &r->dev[i];
        if (!d->used) {
            return d;
        }
        if (best == NULL || (best->present && !d->present) ||
            (best->present == d->present && d->seen < best->seen)) {
            best = d;
        }
    }
    return best;
}

void rec_join(records *r, const uint8_t mac[6], uint32_t now_ms)
{
    rec_device *d = find_mac(r, mac);
    if (d == NULL) {
        d = slot(r);
        memset(d, 0, sizeof(*d));
        d->used = true;
        memcpy(d->mac, mac, 6);
    }
    d->present = true;
    d->joined_ms = now_ms;
    d->seen = ++r->seen;
}

void rec_leave(records *r, const uint8_t mac[6], uint32_t now_ms)
{
    rec_device *d = find_mac(r, mac);
    if (d != NULL && d->present) {
        d->present = false;
        d->left_ms = now_ms;
    }
}

void rec_ip(records *r, const uint8_t mac[6], const uint8_t ip[4])
{
    // An address belongs to one station: a station that left gives it up.
    for (int i = 0; i < REC_DEVICES; i++) {
        if (r->dev[i].used && memcmp(r->dev[i].ip, ip, 4) == 0) {
            memset(r->dev[i].ip, 0, 4);
        }
    }
    rec_device *d = find_mac(r, mac);
    if (d != NULL) {
        memcpy(d->ip, ip, 4);
    }
}

void rec_dns(records *r, const uint8_t ip[4], const char *name)
{
    static const uint8_t none[4];
    if (memcmp(ip, none, 4) == 0) {
        return;
    }
    rec_device *d = NULL;
    for (int i = 0; i < REC_DEVICES && d == NULL; i++) {
        if (r->dev[i].used && memcmp(r->dev[i].ip, ip, 4) == 0) {
            d = &r->dev[i];
        }
    }
    if (d == NULL) {
        return;
    }
    char kept[REC_NAME_MAX];
    snprintf(kept, sizeof(kept), "%s", name);
    // Most recent first; a repeated name moves to the front.
    int at = d->name_count < REC_NAMES ? d->name_count : REC_NAMES - 1;
    for (int i = 0; i < d->name_count; i++) {
        if (strcmp(d->names[i], kept) == 0) {
            at = i;
            break;
        }
    }
    if (at == d->name_count) {
        d->name_count++;
    }
    memmove(d->names[1], d->names[0], (size_t)at * REC_NAME_MAX);
    memcpy(d->names[0], kept, REC_NAME_MAX);
    if (strcmp(name, REC_STOCK_NAME) == 0) {
        d->stock = true;
    }
    d->seen = ++r->seen;
}

void rec_expire(records *r, uint32_t now_ms)
{
    for (int i = 0; i < REC_DEVICES; i++) {
        rec_device *d = &r->dev[i];
        if (d->used && !d->present && now_ms - d->left_ms >= REC_LEFT_KEEP_MS) {
            d->used = false;
        }
    }
}

static rec_download *find_dl(records *r, uint32_t id)
{
    for (int i = 0; i < REC_DOWNLOADS; i++) {
        if (id != 0 && r->dl[i].id == id) {
            return &r->dl[i];
        }
    }
    return NULL;
}

uint32_t rec_dl_start(records *r, const uint8_t ip[4], uint32_t total, uint32_t now_ms)
{
    rec_download *d = &r->dl[0];
    for (int i = 1; i < REC_DOWNLOADS; i++) {
        if (r->dl[i].id < d->id) {
            d = &r->dl[i];
        }
    }
    memset(d, 0, sizeof(*d));
    d->id = r->next_id++;
    d->start_ms = now_ms;
    memcpy(d->ip, ip, 4);
    d->total = total;
    d->outcome = DL_RUNNING;
    return d->id;
}

void rec_dl_update(records *r, uint32_t id, const char *mac, uint32_t sent)
{
    rec_download *d = find_dl(r, id);
    if (d != NULL) {
        snprintf(d->mac, sizeof(d->mac), "%s", mac);
        d->sent = sent;
    }
}

void rec_dl_end(records *r, uint32_t id, rec_outcome outcome, uint32_t now_ms)
{
    rec_download *d = find_dl(r, id);
    if (d != NULL) {
        d->outcome = outcome;
        d->end_ms = now_ms;
    }
}

const char *rec_outcome_name(rec_outcome o)
{
    switch (o) {
    case DL_RUNNING: return "in progress";
    case DL_OK: return "sent every byte";
    case DL_STOPPED: return "stopped early";
    case DL_REFUSED: return "refused: no image";
    }
    return "?";
}

void rec_mac_text(const uint8_t mac[6], char out[REC_MAC_TEXT])
{
    snprintf(out, REC_MAC_TEXT, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
}

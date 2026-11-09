// Device and download records for the page (installer/SPEC.md, "Page"): the
// stations on the hotspot with the names they looked up, and the last WiFi
// install sessions. Plain C, no platform dependencies; times are milliseconds
// from any monotonic clock. The caller serialises access.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_DEVICES 16
#define REC_NAMES 8
#define REC_NAME_MAX 64           // name text kept, with the NUL
#define REC_DOWNLOADS 10
#define REC_LEFT_KEEP_MS 600000u  // a station that left stays listed this long
#define REC_STOCK_NAME "iotc.kuailece.cn"
#define REC_MAC_TEXT 18           // "AA:BB:CC:DD:EE:FF" and the NUL

typedef struct {
    bool used;
    bool present;              // on the hotspot now
    bool stock;                // looked up REC_STOCK_NAME: a stock K-Radio
    uint8_t mac[6];
    uint8_t ip[4];             // 0.0.0.0 until DHCP assigns one
    uint32_t joined_ms;
    uint32_t left_ms;
    uint32_t seen;             // order of the last join or lookup, for eviction
    uint8_t name_count;
    char names[REC_NAMES][REC_NAME_MAX];  // most recent first, no repeats
} rec_device;

typedef enum {
    DL_RUNNING,  // in progress
    DL_OK,       // sent every byte
    DL_STOPPED,  // stopped early
    DL_REFUSED,  // refused: no image
} rec_outcome;

typedef struct {
    uint32_t id;               // 1, 2, ... in start order; 0 marks an unused slot
    uint32_t start_ms;
    uint32_t end_ms;
    uint8_t ip[4];
    char mac[REC_MAC_TEXT];    // as the radio sent it; empty until it does
    uint32_t sent;
    uint32_t total;
    rec_outcome outcome;
} rec_download;

typedef struct {
    rec_device dev[REC_DEVICES];
    rec_download dl[REC_DOWNLOADS];
    uint32_t next_id;
    uint32_t seen;
} records;

void rec_init(records *r);

// A station joined or left the hotspot.
void rec_join(records *r, const uint8_t mac[6], uint32_t now_ms);
void rec_leave(records *r, const uint8_t mac[6], uint32_t now_ms);
// DHCP gave a station an address.
void rec_ip(records *r, const uint8_t mac[6], const uint8_t ip[4]);
// A DNS query from ip. Recorded on the device with that address, if any.
void rec_dns(records *r, const uint8_t ip[4], const char *name);
// Drop stations that left more than REC_LEFT_KEEP_MS ago.
void rec_expire(records *r, uint32_t now_ms);

// A WiFi install session from ip started. Returns its id; the oldest record
// makes room once REC_DOWNLOADS are kept.
uint32_t rec_dl_start(records *r, const uint8_t ip[4], uint32_t total, uint32_t now_ms);
// Progress and end of session id; ignored once it has been dropped.
void rec_dl_update(records *r, uint32_t id, const char *mac, uint32_t sent);
void rec_dl_end(records *r, uint32_t id, rec_outcome outcome, uint32_t now_ms);
const char *rec_outcome_name(rec_outcome o);
// "AA:BB:CC:DD:EE:FF"
void rec_mac_text(const uint8_t mac[6], char out[REC_MAC_TEXT]);

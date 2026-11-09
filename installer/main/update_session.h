// One session of the stock update protocol, server side (installer/SPEC.md,
// "WiFi install"). Plain C, no platform dependencies: the caller feeds received
// bytes, peer closes and the time, and asks update_next() what to do.
//
//   1. Wait for the 6 bytes "update" (US_CMD_MS, else close).
//   2. Close when there is no image the stock updater can take.
//   3. Send "999,<size>", no terminator.
//   4. Read the MAC: stream once 17 bytes have arrived, or US_MAC_GAP_MS after
//      the first byte; close if no byte comes within US_MAC_FIRST_MS.
//   5. Stream the image, then wait for the peer to close, at most US_DRAIN_MS.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define US_CMD_MS 1000
// The stock client reads the header with readStringUntil('\r') on a WiFiClient,
// whose Stream timeout is 5000 ms, so its MAC comes about 5 s after the header.
#define US_MAC_FIRST_MS 10000
#define US_MAC_GAP_MS 300
#define US_DRAIN_MS 20000
#define US_MAC_LEN 17
// The largest image the stock Update.begin() accepts: 0x3fb000 minus the largest
// known stock app (440,208 B) rounded up to 4 KB.
#define US_IMAGE_MAX 3731456u
// Byte 0 of an image Update.end() accepts.
#define US_IMAGE_MAGIC 0xE9
// After the copy to 0x0, eboot boots the image header at 0x1000. An install
// image (radio/tools/make_install_image.py) has a mini image there: magic, one
// segment at US_STUB_ADDR, entry inside that segment. US_HEAD_LEN bytes from
// US_HEAD_AT hold its image and segment headers.
#define US_HEAD_AT 0x1000u
#define US_HEAD_LEN 16u
#define US_STUB_ADDR 0x40100000u

typedef enum {
    US_NONE,         // nothing to do now; call again on the next event or tick
    US_SEND_HEADER,  // send header/header_len, then call update_header_sent()
    US_STREAM,       // send image bytes from offset sent, then call update_sent()
    US_CLOSE,        // close the connection; outcome says how it ended
} update_action;

typedef enum {
    US_OK,           // every image byte was sent
    US_NO_COMMAND,   // no "update" within US_CMD_MS
    US_BAD_COMMAND,  // other bytes than "update"
    US_NO_MAC,       // no MAC byte within US_MAC_FIRST_MS
    US_PEER_CLOSED,  // the peer closed or the connection failed before the last byte
    US_REFUSED,      // no image the stock updater can take
} update_outcome;

typedef enum { ST_CMD, ST_HEADER, ST_MAC, ST_STREAM, ST_DRAIN, ST_DONE } update_state;

typedef struct {
    update_state state;
    update_outcome outcome;
    uint32_t image_len;
    bool image_ok;
    uint32_t sent;
    uint32_t t_state;      // when the current state began
    uint32_t t_mac_first;  // when the first MAC byte arrived
    uint8_t cmd_len;
    uint8_t mac_len;
    bool peer_closed;
    char mac[US_MAC_LEN + 1];
    char header[16];
    size_t header_len;
} update_session;

// NULL when the stock updater takes an image of len bytes whose first byte is
// first_byte, and eboot boots it after the copy: head is the image's US_HEAD_LEN
// bytes at US_HEAD_AT (ignored when the image is shorter). Otherwise why not.
const char *update_image_problem(uint32_t len, uint8_t first_byte, const uint8_t head[US_HEAD_LEN]);
// image_ok: there is a complete carried image and update_image_problem() finds
// no problem.
void update_start(update_session *s, uint32_t image_len, bool image_ok, uint32_t now_ms);
void update_received(update_session *s, const uint8_t *data, size_t len, uint32_t now_ms);
void update_peer_closed(update_session *s);
update_action update_next(update_session *s, uint32_t now_ms);
void update_header_sent(update_session *s, uint32_t now_ms);
void update_sent(update_session *s, size_t n);
const char *update_outcome_name(update_outcome o);

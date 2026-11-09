// The installer's DNS and update servers on BSD sockets. The same code runs on
// lwIP in the C3 firmware and on POSIX sockets in the host program
// (installer/host/), which the tests drive.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "update_session.h"

typedef struct {
    uint32_t bind_addr;      // network byte order (the hotspot address on the C3, loopback on the host)
    uint16_t dns_port;
    uint16_t update_port;
    uint8_t answer_ip[4];    // the A record every name resolves to
} server_config;

// Provided by the platform (installer/main/main.c, installer/host/host_main.c).
uint32_t platform_now_ms(void);
void platform_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// A DNS query from ip was answered.
void platform_dns_query(const uint8_t ip[4], const char *name);
// The carried image for one session. Returns NULL when it is complete, else why
// the session cannot have it; len gets its flat length. The image stays
// unchanged until platform_image_release().
const char *platform_image_hold(uint32_t *len);
// Read len bytes of the flat image at offset. Returns false on a read error.
bool platform_image_read(uint32_t offset, uint8_t *buf, size_t len);
void platform_image_release(void);
// A session from ip started (once), made progress, or ended (done).
void platform_session(const uint8_t ip[4], const update_session *us, bool done);

// Why the WiFi install cannot send the complete carried image of len bytes (read
// with platform_image_read()), or NULL.
const char *server_image_problem(uint32_t len);

// Binds both servers and serves forever. Returns only when a socket cannot be
// set up, with a negative value.
int server_run(const server_config *cfg);

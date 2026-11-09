// Host build of the installer's servers, for the tests: the same DNS and update
// code as the C3 firmware, on POSIX sockets bound to 127.0.0.1, serving the
// carried image from a stage file (tools/make_stage.py) the way the C3 serves
// it from its stage partition.
//
//   installer-host --stage FILE --dns-port N --update-port N
//
// The image is complete when the header is valid and every data region has its
// present flag; the C3 also checks each region's SHA-256 at boot, which the
// tests do on the file instead. Logs to stdout, one line per event.

#include <arpa/inet.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server.h"
#include "stage_layout.h"

static unsigned char *s_stage;
static long s_stage_len;
static sl_manifest_t s_manifest;

uint32_t platform_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void platform_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

void platform_dns_query(const uint8_t ip[4], const char *name)
{
    (void)ip;
    (void)name;
}

void platform_session(const uint8_t ip[4], const update_session *us, bool done)
{
    (void)ip;
    (void)us;
    (void)done;
}

bool platform_image_read(uint32_t offset, uint8_t *buf, size_t len)
{
    while (len > 0) {
        sl_span_t s = sl_flat_span(&s_manifest, offset, (uint32_t)len);
        if (s.len == 0 || (!s.fill && (long)(s.stage + s.len) > s_stage_len))
            return false;
        if (s.fill)
            memset(buf, 0xFF, s.len);
        else
            memcpy(buf, s_stage + s.stage, s.len);
        buf += s.len;
        offset += s.len;
        len -= s.len;
    }
    return true;
}

const char *platform_image_hold(uint32_t *len)
{
    *len = sl_total_length(&s_manifest);
    return sl_all_flagged(&s_manifest) ? NULL : "no complete image";
}

void platform_image_release(void) {}

static unsigned char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    unsigned char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (*len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)*len);
        if (buf && fread(buf, 1, (size_t)*len, f) != (size_t)*len) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char *stage_path = NULL;
    long dns_port = 0, update_port = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--stage"))
            stage_path = argv[i + 1];
        else if (!strcmp(argv[i], "--dns-port"))
            dns_port = strtol(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--update-port"))
            update_port = strtol(argv[i + 1], NULL, 10);
    }
    if (!stage_path || dns_port <= 0 || dns_port > 65535 || update_port <= 0 || update_port > 65535) {
        fprintf(stderr, "usage: %s --stage FILE --dns-port N --update-port N\n", argv[0]);
        return 2;
    }
    s_stage = read_file(stage_path, &s_stage_len);
    if (!s_stage || s_stage_len < (long)SL_HEADER_SIZE) {
        fprintf(stderr, "cannot read %s\n", stage_path);
        return 1;
    }
    if (!sl_header_decode(s_stage, &s_manifest, SL_STAGE_MAX))
        platform_log("stage: no valid header");
    else
        platform_log("stage: \"%s\", %lu regions, %lu bytes, %lu of %lu data regions present", s_manifest.label,
                     (unsigned long)s_manifest.count, (unsigned long)sl_total_length(&s_manifest),
                     (unsigned long)sl_flagged_regions(&s_manifest), (unsigned long)sl_data_regions(&s_manifest));
    signal(SIGPIPE, SIG_IGN);  // a peer that resets mid-transfer shows up as a send error
    server_config cfg = {
        .bind_addr = htonl(INADDR_LOOPBACK),
        .dns_port = (uint16_t)dns_port,
        .update_port = (uint16_t)update_port,
        .answer_ip = {192, 168, 4, 1},
    };
    return server_run(&cfg) < 0 ? 1 : 0;
}

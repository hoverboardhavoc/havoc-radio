// Host tests for installer/main/update_session.c (installer/SPEC.md, Tests 1):
// the session's decisions with a simulated clock. The socket-level cases
// (streaming, disconnects, a second connection) run against the host program
// in test_protocol.py.

#include <string.h>

#include "check.h"
#include "update_session.h"

static void send_cmd(update_session *s, uint32_t now)
{
    update_received(s, (const uint8_t *)"update", 6, now);
}

// The mini image make_install_image.py puts at 0x1000: magic, 1 segment, DIO,
// 4 MB / 40 MHz, entry 0x40100010; segment at 0x40100000, 0x3c bytes.
static const uint8_t INSTALL_HEAD[US_HEAD_LEN] = {0xE9, 0x01, 0x02, 0x40, 0x10, 0x00, 0x10, 0x40,
                                                  0x00, 0x00, 0x10, 0x40, 0x3c, 0x00, 0x00, 0x00};

static void test_problem(void)
{
    uint8_t h[US_HEAD_LEN];
    CHECK(update_image_problem(770048, 0xE9, INSTALL_HEAD) == NULL);
    CHECK(update_image_problem(US_IMAGE_MAX, 0xE9, INSTALL_HEAD) == NULL);
    CHECK(update_image_problem(US_IMAGE_MAX + 1, 0xE9, INSTALL_HEAD) != NULL);
    CHECK(update_image_problem(4u << 20, 0xE9, INSTALL_HEAD) != NULL);  // the 4 MB stock backup
    CHECK(strcmp(update_image_problem(770048, 0xFF, INSTALL_HEAD), "does not start with 0xE9") == 0);
    CHECK(update_image_problem(0, 0xE9, INSTALL_HEAD) != NULL);
    CHECK(update_image_problem(US_HEAD_AT + US_HEAD_LEN, 0xE9, INSTALL_HEAD) != NULL);  // segment past the end

    // A plain build: the RTOS bootloader's code at 0x1000, not a mini image.
    static const uint8_t plain[US_HEAD_LEN] = {0x12, 0xc1, 0xf0, 0x09, 0x31, 0xc9, 0x21, 0x0d,
                                               0x02, 0x21, 0xfb, 0xff, 0x01, 0xfb, 0xff, 0xc0};
    CHECK(strcmp(update_image_problem(770048, 0xE9, plain), "not an install image") == 0);
    // Each field of the mini image matters.
    static const struct {
        int at;
        uint8_t value;
    } bad[] = {
        {0, 0xEA},   // magic
        {1, 2},      // two segments
        {10, 0x11},  // segment at 0x40110000
        {4, 0x00},   // entry 0x40100000 is inside; 0x400ff000 below:
        {6, 0x0f},   // entry 0x400f0010, below the segment
        {4, 0x3c},   // entry at the segment end
        {12, 0x00},  // empty segment
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        memcpy(h, INSTALL_HEAD, sizeof(h));
        h[bad[k].at] = bad[k].value;
        bool entry_at_start = bad[k].at == 4 && bad[k].value == 0x00;
        CHECK((update_image_problem(770048, 0xE9, h) == NULL) == entry_at_start);
    }
}

static void test_ok(void)
{
    update_session s;
    update_start(&s, 770048, true, 1000);
    CHECK(update_next(&s, 1000) == US_NONE);
    send_cmd(&s, 1100);
    CHECK(update_next(&s, 1100) == US_SEND_HEADER);
    CHECK(s.header_len == strlen("999,770048") && memcmp(s.header, "999,770048", s.header_len) == 0);
    update_header_sent(&s, 1100);
    // The MAC comes about 5 s later; nothing before.
    CHECK(update_next(&s, 6000) == US_NONE);
    update_received(&s, (const uint8_t *)"5C:CF:7F:01:02:03", 17, 6100);
    CHECK(update_next(&s, 6100) == US_STREAM);
    CHECK(strcmp(s.mac, "5C:CF:7F:01:02:03") == 0);
    update_sent(&s, 770000);
    CHECK(update_next(&s, 6200) == US_STREAM);
    update_sent(&s, 48);
    CHECK(update_next(&s, 6300) == US_NONE);  // drain: wait for the peer to close
    update_peer_closed(&s);
    CHECK(update_next(&s, 6400) == US_CLOSE);
    CHECK(s.outcome == US_OK);
}

static void test_drain_limit(void)
{
    update_session s;
    update_start(&s, 4096, true, 0);
    send_cmd(&s, 0);
    CHECK(update_next(&s, 0) == US_SEND_HEADER);
    update_header_sent(&s, 0);
    update_received(&s, (const uint8_t *)"5C:CF:7F:01:02:03", 17, 10);
    CHECK(update_next(&s, 10) == US_STREAM);
    update_sent(&s, 4096);
    CHECK(update_next(&s, 20) == US_NONE);
    CHECK(update_next(&s, 20 + US_DRAIN_MS - 1) == US_NONE);
    CHECK(update_next(&s, 20 + US_DRAIN_MS) == US_CLOSE);
    CHECK(s.outcome == US_OK);
}

static void test_short_mac(void)
{
    // A MAC shorter than 17 bytes: stream US_MAC_GAP_MS after its first byte.
    update_session s;
    update_start(&s, 4096, true, 0);
    send_cmd(&s, 0);
    CHECK(update_next(&s, 0) == US_SEND_HEADER);
    update_header_sent(&s, 0);
    update_received(&s, (const uint8_t *)"AB:CD", 5, 5000);
    CHECK(update_next(&s, 5000 + US_MAC_GAP_MS - 1) == US_NONE);
    CHECK(update_next(&s, 5000 + US_MAC_GAP_MS) == US_STREAM);
    CHECK(strcmp(s.mac, "AB:CD") == 0);
}

static void test_no_mac(void)
{
    update_session s;
    update_start(&s, 4096, true, 0);
    send_cmd(&s, 0);
    CHECK(update_next(&s, 0) == US_SEND_HEADER);
    update_header_sent(&s, 0);
    CHECK(update_next(&s, US_MAC_FIRST_MS - 1) == US_NONE);
    CHECK(update_next(&s, US_MAC_FIRST_MS) == US_CLOSE);
    CHECK(s.outcome == US_NO_MAC);
}

static void test_refused(void)
{
    // No complete image (or one the stock updater cannot take): the command is
    // read, then the connection closes with no header.
    update_session s;
    update_start(&s, 0, false, 0);
    CHECK(update_next(&s, 10) == US_NONE);
    send_cmd(&s, 20);
    CHECK(update_next(&s, 20) == US_CLOSE);
    CHECK(s.outcome == US_REFUSED);
    CHECK(strcmp(update_outcome_name(s.outcome), "refused: no image") == 0);
}

static void test_command_errors(void)
{
    update_session s;
    update_start(&s, 4096, true, 0);
    CHECK(update_next(&s, US_CMD_MS - 1) == US_NONE);
    CHECK(update_next(&s, US_CMD_MS) == US_CLOSE);
    CHECK(s.outcome == US_NO_COMMAND);

    update_start(&s, 4096, true, 0);
    update_received(&s, (const uint8_t *)"GET / HTTP/1.1", 14, 5);
    CHECK(update_next(&s, 5) == US_CLOSE);
    CHECK(s.outcome == US_BAD_COMMAND);

    // The command split over several reads.
    update_start(&s, 4096, true, 0);
    update_received(&s, (const uint8_t *)"upd", 3, 5);
    CHECK(update_next(&s, 5) == US_NONE);
    update_received(&s, (const uint8_t *)"ate", 3, 6);
    CHECK(update_next(&s, 6) == US_SEND_HEADER);
}

static void test_peer_closed(void)
{
    update_session s;
    update_start(&s, 1 << 20, true, 0);
    send_cmd(&s, 0);
    CHECK(update_next(&s, 0) == US_SEND_HEADER);
    update_header_sent(&s, 0);
    update_received(&s, (const uint8_t *)"5C:CF:7F:01:02:03", 17, 10);
    CHECK(update_next(&s, 10) == US_STREAM);
    update_sent(&s, 65536);
    update_peer_closed(&s);
    CHECK(update_next(&s, 20) == US_CLOSE);
    CHECK(s.outcome == US_PEER_CLOSED && s.sent == 65536);
}

int main(void)
{
    test_problem();
    test_ok();
    test_drain_limit();
    test_short_mac();
    test_no_mac();
    test_refused();
    test_command_errors();
    test_peer_closed();
    return check_report("update_session");
}

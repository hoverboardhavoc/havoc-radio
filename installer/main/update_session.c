#include "update_session.h"

#include <stdio.h>
#include <string.h>

static const char CMD[] = "update";

static void enter(update_session *s, update_state st, uint32_t now_ms)
{
    s->state = st;
    s->t_state = now_ms;
}

static update_action finish(update_session *s, update_outcome o)
{
    if (s->state != ST_DONE) {
        s->outcome = o;
        s->state = ST_DONE;
    }
    return US_CLOSE;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

const char *update_image_problem(uint32_t len, uint8_t first_byte, const uint8_t head[US_HEAD_LEN])
{
    if (len > US_IMAGE_MAX)
        return "longer than the stock updater takes";
    if (first_byte != US_IMAGE_MAGIC)
        return "does not start with 0xE9";
    // eboot's image header (magic, segment count, flash mode, size/freq, entry),
    // then the first segment's address and size.
    if (len < US_HEAD_AT + US_HEAD_LEN || head[0] != US_IMAGE_MAGIC || head[1] != 1)
        return "not an install image";
    uint32_t entry = le32(head + 4), addr = le32(head + 8), size = le32(head + 12);
    if (addr != US_STUB_ADDR || size == 0 || size > len - US_HEAD_AT - US_HEAD_LEN || entry < addr ||
        entry - addr >= size)
        return "not an install image";
    return NULL;
}

void update_start(update_session *s, uint32_t image_len, bool image_ok, uint32_t now_ms)
{
    memset(s, 0, sizeof *s);
    s->image_len = image_len;
    s->image_ok = image_ok;
    // Version 999: the stock firmware stops only on an equal version string, and
    // one that compares numbers sees a newer version.
    s->header_len = (size_t)snprintf(s->header, sizeof s->header, "999,%lu", (unsigned long)image_len);
    enter(s, ST_CMD, now_ms);
}

void update_received(update_session *s, const uint8_t *data, size_t len, uint32_t now_ms)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (s->state == ST_CMD) {
            if (c != (uint8_t)CMD[s->cmd_len]) {
                finish(s, US_BAD_COMMAND);
                return;
            }
            s->cmd_len++;
            if (s->cmd_len == sizeof CMD - 1)
                enter(s, ST_HEADER, now_ms);
        } else if (s->state == ST_HEADER || s->state == ST_MAC) {
            if (s->mac_len == 0)
                s->t_mac_first = now_ms;
            if (s->mac_len < US_MAC_LEN)
                s->mac[s->mac_len++] = (c > 0x20 && c < 0x7F) ? (char)c : '?';
        }
        // Bytes during the stream and after it are ignored.
    }
}

void update_peer_closed(update_session *s) { s->peer_closed = true; }

update_action update_next(update_session *s, uint32_t now_ms)
{
    uint32_t in_state = now_ms - s->t_state;
    switch (s->state) {
    case ST_CMD:
        if (s->peer_closed)
            return finish(s, US_PEER_CLOSED);
        return in_state >= US_CMD_MS ? finish(s, US_NO_COMMAND) : US_NONE;
    case ST_HEADER:
        if (!s->image_ok)
            return finish(s, US_REFUSED);
        return s->peer_closed ? finish(s, US_PEER_CLOSED) : US_SEND_HEADER;
    case ST_MAC:
        if (s->mac_len == US_MAC_LEN || (s->mac_len > 0 && now_ms - s->t_mac_first >= US_MAC_GAP_MS)) {
            enter(s, ST_STREAM, now_ms);
            return update_next(s, now_ms);
        }
        if (s->peer_closed)
            return finish(s, US_PEER_CLOSED);
        return (s->mac_len == 0 && in_state >= US_MAC_FIRST_MS) ? finish(s, US_NO_MAC) : US_NONE;
    case ST_STREAM:
        if (s->sent >= s->image_len) {
            enter(s, ST_DRAIN, now_ms);
            return update_next(s, now_ms);
        }
        return s->peer_closed ? finish(s, US_PEER_CLOSED) : US_STREAM;
    case ST_DRAIN:
        return (s->peer_closed || in_state >= US_DRAIN_MS) ? finish(s, US_OK) : US_NONE;
    case ST_DONE:
        break;
    }
    return US_CLOSE;
}

void update_header_sent(update_session *s, uint32_t now_ms)
{
    if (s->state == ST_HEADER)
        enter(s, ST_MAC, now_ms);
}

void update_sent(update_session *s, size_t n)
{
    if (s->state == ST_STREAM)
        s->sent += (uint32_t)n;
}

const char *update_outcome_name(update_outcome o)
{
    switch (o) {
    case US_OK: return "sent every byte";
    case US_NO_COMMAND: return "no update command";
    case US_BAD_COMMAND: return "bad command";
    case US_NO_MAC: return "no MAC";
    case US_PEER_CLOSED: return "peer closed early";
    case US_REFUSED: return "refused: no image";
    }
    return "?";
}

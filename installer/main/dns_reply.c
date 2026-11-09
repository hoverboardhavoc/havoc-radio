#include "dns_reply.h"

#include <string.h>

#define HDR_LEN 12
#define TYPE_A 1
#define CLASS_IN 1

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t dns_reply(const uint8_t *query, size_t len, const uint8_t addr[4], uint8_t *reply, size_t cap,
                 char name[DNS_NAME_MAX], uint16_t *qtype)
{
    if (len < HDR_LEN)
        return 0;
    uint16_t flags = get16(query + 2);
    if (flags & 0x8000)              // a response
        return 0;
    if ((flags >> 11) & 0xF)         // not a standard query
        return 0;
    if (get16(query + 4) != 1)       // QDCOUNT
        return 0;

    // Question name: uncompressed labels, 255 octets at most with the final zero,
    // so the labels and their length bytes take at most 254.
    size_t pos = HDR_LEN;
    size_t n = 0;
    for (;;) {
        if (pos >= len)
            return 0;
        uint8_t l = query[pos++];
        if (l == 0)
            break;
        if (l > 63 || pos + l > len || pos - HDR_LEN + l > 254)
            return 0;
        if (n)
            name[n++] = '.';
        for (uint8_t i = 0; i < l; i++) {
            uint8_t c = query[pos + i];
            name[n++] = (c > 0x20 && c < 0x7F) ? (char)c : '?';
        }
        pos += l;
    }
    name[n] = '\0';
    if (pos + 4 > len)
        return 0;
    uint16_t type = get16(query + pos);
    uint16_t qclass = get16(query + pos + 2);
    size_t qend = pos + 4;

    int answer = type == TYPE_A && qclass == CLASS_IN;
    size_t out_len = qend + (answer ? 16 : 0);
    if (out_len > cap)
        return 0;

    memcpy(reply, query, qend);
    // QR, opcode 0, AA, RD copied, RA, RCODE 0 (NOERROR).
    put16(reply + 2, (uint16_t)(0x8000 | 0x0400 | (flags & 0x0100) | 0x0080));
    put16(reply + 6, answer ? 1 : 0);  // ANCOUNT
    put16(reply + 8, 0);               // NSCOUNT
    put16(reply + 10, 0);              // ARCOUNT
    if (answer) {
        uint8_t *a = reply + qend;
        put16(a, 0xC00C);              // name: pointer to the question
        put16(a + 2, TYPE_A);
        put16(a + 4, CLASS_IN);
        put16(a + 6, 0);
        put16(a + 8, DNS_TTL_S);
        put16(a + 10, 4);
        memcpy(a + 12, addr, 4);
    }
    *qtype = type;
    return out_len;
}

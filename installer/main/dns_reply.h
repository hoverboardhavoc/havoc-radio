// DNS replies for the installer hotspot: every A query of class IN gets one A
// record with the hotspot's address. Plain C, no platform dependencies.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define DNS_TTL_S 60
// Longest name text: 255 octets on the wire give at most 253 characters.
#define DNS_NAME_MAX 254

// Builds the reply to one UDP DNS packet into reply (cap bytes).
// - Standard query (QR 0, opcode 0, one question) of class IN and type A: one A
//   record with addr, TTL DNS_TTL_S.
// - Standard query of any other type or class: NOERROR with no answers.
// - Anything else, or a packet that does not parse: dropped.
// Returns the reply length, or 0 to drop. On a reply, name holds the query name
// (dotted, NUL-terminated, non-printable bytes as '?') and *qtype its type.
size_t dns_reply(const uint8_t *query, size_t len, const uint8_t addr[4], uint8_t *reply, size_t cap,
                 char name[DNS_NAME_MAX], uint16_t *qtype);

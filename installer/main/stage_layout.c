#include "stage_layout.h"

#include <string.h>

const char *sl_err_str(sl_err_t err)
{
    switch (err) {
    case SL_OK: return "ok";
    case SL_ERR_COUNT: return "region count out of range";
    case SL_ERR_ALIGN: return "region target or length not 4 KB-aligned";
    case SL_ERR_RANGE: return "region past the end of target flash";
    case SL_ERR_OVERLAP: return "regions overlap";
    case SL_ERR_TOO_BIG: return "data regions do not fit in the stage";
    case SL_ERR_REGION: return "no such region";
    case SL_ERR_ERASE: return "region is erase-only";
    case SL_ERR_OFFSET: return "part offset not 4 KB-aligned";
    case SL_ERR_SIZE: return "part size invalid";
    }
    return "unknown";
}

sl_err_t sl_layout(sl_manifest_t *m, uint32_t stage_size)
{
    if (m->count == 0 || m->count > SL_MAX_REGIONS) {
        return SL_ERR_COUNT;
    }
    if (stage_size > SL_STAGE_MAX) {
        stage_size = SL_STAGE_MAX;
    }
    uint32_t stage = SL_HEADER_SIZE;
    for (uint32_t i = 0; i < m->count; i++) {
        sl_region_t *r = &m->regions[i];
        if (r->length == 0 || r->target % SL_SECTOR_SIZE || r->length % SL_SECTOR_SIZE) {
            return SL_ERR_ALIGN;
        }
        if (r->target >= SL_TARGET_MAX || r->length > SL_TARGET_MAX - r->target) {
            return SL_ERR_RANGE;
        }
        for (uint32_t j = 0; j < i; j++) {
            const sl_region_t *o = &m->regions[j];
            if (r->target < o->target + o->length && o->target < r->target + r->length) {
                return SL_ERR_OVERLAP;
            }
        }
        if (sl_is_erase(r)) {
            r->stage = 0;
            r->flags = SL_FLAG_ERASE;
            continue;
        }
        if (stage > stage_size || r->length > stage_size - stage) {
            return SL_ERR_TOO_BIG;
        }
        r->stage = stage;
        r->flags &= SL_FLAG_PRESENT;
        stage += r->length;
    }
    return SL_OK;
}

bool sl_manifest_equal(const sl_manifest_t *a, const sl_manifest_t *b)
{
    if (a->count != b->count || a->count > SL_MAX_REGIONS) {
        return false;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        const sl_region_t *x = &a->regions[i];
        const sl_region_t *y = &b->regions[i];
        if (x->target != y->target || x->length != y->length || sl_is_erase(x) != sl_is_erase(y) ||
            memcmp(x->sha256, y->sha256, sizeof(x->sha256)) != 0) {
            return false;
        }
    }
    return true;
}

void sl_set_label(sl_manifest_t *m, const char *label)
{
    size_t n = strlen(label);
    if (n > SL_LABEL_MAX) {
        n = SL_LABEL_MAX;
        // Back up over UTF-8 continuation bytes to the start of the cut character.
        while (n > 0 && ((uint8_t)label[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(m->label, label, n);
    m->label[n] = '\0';
}

// ---- Header ------------------------------------------------------------------

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t sl_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t sl_total_length(const sl_manifest_t *m)
{
    uint32_t end = 0;
    for (uint32_t i = 0; i < m->count && i < SL_MAX_REGIONS; i++) {
        const sl_region_t *r = &m->regions[i];
        if (r->target + r->length > end) {
            end = r->target + r->length;
        }
    }
    return end;
}

void sl_header_encode(const sl_manifest_t *m, uint8_t *out)
{
    memset(out, 0xFF, SL_HEADER_SIZE);
    memset(out, 0, SL_HEADER_CRC_AT);
    memcpy(out, SL_HEADER_MAGIC, 4);
    put32(out + 4, SL_HEADER_VERSION);
    put32(out + 8, m->count);
    put32(out + 12, sl_total_length(m));
    memcpy(out + 16, m->label, strnlen(m->label, SL_LABEL_MAX));
    for (uint32_t i = 0; i < m->count; i++) {
        const sl_region_t *r = &m->regions[i];
        uint8_t *e = out + SL_HEADER_REGIONS_AT + i * SL_HEADER_REGION_SIZE;
        put32(e, r->target);
        put32(e + 4, r->length);
        put32(e + 8, r->stage);
        put32(e + 12, r->flags);
        memcpy(e + 16, r->sha256, 32);
    }
    put32(out + SL_HEADER_CRC_AT, sl_crc32(out, SL_HEADER_CRC_AT));
}

bool sl_header_decode(const uint8_t *in, sl_manifest_t *m, uint32_t stage_size)
{
    memset(m, 0, sizeof(*m));
    uint32_t count = get32(in + 8);
    if (memcmp(in, SL_HEADER_MAGIC, 4) != 0 || get32(in + 4) != SL_HEADER_VERSION ||
        count == 0 || count > SL_MAX_REGIONS ||
        get32(in + SL_HEADER_CRC_AT) != sl_crc32(in, SL_HEADER_CRC_AT)) {
        return false;
    }
    m->count = count;
    memcpy(m->label, in + 16, SL_LABEL_MAX);
    m->label[SL_LABEL_MAX] = '\0';
    uint32_t stored_stage[SL_MAX_REGIONS];
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = in + SL_HEADER_REGIONS_AT + i * SL_HEADER_REGION_SIZE;
        sl_region_t *r = &m->regions[i];
        r->target = get32(e);
        r->length = get32(e + 4);
        stored_stage[i] = get32(e + 8);
        r->flags = get32(e + 12);
        memcpy(r->sha256, e + 16, 32);
        if (r->flags & ~(SL_FLAG_ERASE | SL_FLAG_PRESENT) ||
            r->flags == (SL_FLAG_ERASE | SL_FLAG_PRESENT)) {
            goto bad;
        }
    }
    if (sl_layout(m, stage_size) != SL_OK || get32(in + 12) != sl_total_length(m)) {
        goto bad;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (m->regions[i].stage != stored_stage[i]) {
            goto bad;
        }
    }
    return true;
bad:
    memset(m, 0, sizeof(*m));
    return false;
}

bool sl_all_flagged(const sl_manifest_t *m)
{
    return m->count > 0 && sl_flagged_regions(m) == sl_data_regions(m);
}

uint32_t sl_data_regions(const sl_manifest_t *m)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        n += !sl_is_erase(&m->regions[i]);
    }
    return n;
}

uint32_t sl_flagged_regions(const sl_manifest_t *m)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        n += !sl_is_erase(&m->regions[i]) && (m->regions[i].flags & SL_FLAG_PRESENT);
    }
    return n;
}

sl_span_t sl_flat_span(const sl_manifest_t *m, uint32_t offset, uint32_t max)
{
    sl_span_t s = {.len = 0, .fill = true, .stage = 0};
    uint32_t end = sl_total_length(m);
    if (offset >= end || max == 0) {
        return s;
    }
    // The fill runs to the next region that starts after offset, or the end.
    uint32_t next = end;
    for (uint32_t i = 0; i < m->count; i++) {
        const sl_region_t *r = &m->regions[i];
        if (offset >= r->target && offset < r->target + r->length) {
            next = r->target + r->length;
            if (!sl_is_erase(r)) {
                s.fill = false;
                s.stage = r->stage + (offset - r->target);
            }
            break;
        }
        if (r->target > offset && r->target < next) {
            next = r->target;
        }
    }
    s.len = next - offset < max ? next - offset : max;
    return s;
}

// ---- Parts -----------------------------------------------------------------------

uint32_t sl_part_count(const sl_region_t *r)
{
    if (sl_is_erase(r)) {
        return 0;
    }
    return (r->length + SL_PART_SIZE - 1) / SL_PART_SIZE;
}

sl_err_t sl_part_check(const sl_manifest_t *m, uint32_t region, uint32_t offset,
                       uint32_t len, uint32_t *stage_addr)
{
    if (region >= m->count) {
        return SL_ERR_REGION;
    }
    const sl_region_t *r = &m->regions[region];
    if (sl_is_erase(r)) {
        return SL_ERR_ERASE;
    }
    if (offset % SL_SECTOR_SIZE) {
        return SL_ERR_OFFSET;
    }
    if (len == 0 || len > SL_PART_SIZE || len % SL_SECTOR_SIZE || offset >= r->length ||
        len > r->length - offset) {
        return SL_ERR_SIZE;
    }
    *stage_addr = r->stage + offset;
    return SL_OK;
}

void sl_presence_clear(sl_presence_t *p)
{
    memset(p->bits, 0, sizeof(p->bits));
}

void sl_presence_set(sl_presence_t *p, uint32_t stage_addr, uint32_t len, bool present)
{
    uint32_t first = stage_addr / SL_SECTOR_SIZE;
    uint32_t end = (stage_addr + len + SL_SECTOR_SIZE - 1) / SL_SECTOR_SIZE;
    for (uint32_t s = first; s < end && s < SL_STAGE_SECTORS; s++) {
        if (present) {
            p->bits[s / 8] |= (uint8_t)(1u << (s % 8));
        } else {
            p->bits[s / 8] &= (uint8_t)~(1u << (s % 8));
        }
    }
}

void sl_presence_from_flags(const sl_manifest_t *m, sl_presence_t *p)
{
    sl_presence_clear(p);
    for (uint32_t i = 0; i < m->count; i++) {
        const sl_region_t *r = &m->regions[i];
        if (!sl_is_erase(r) && (r->flags & SL_FLAG_PRESENT)) {
            sl_presence_set(p, r->stage, r->length, true);
        }
    }
}

static bool sectors_present(const sl_presence_t *p, uint32_t stage_addr, uint32_t len)
{
    uint32_t first = stage_addr / SL_SECTOR_SIZE;
    uint32_t end = (stage_addr + len + SL_SECTOR_SIZE - 1) / SL_SECTOR_SIZE;
    for (uint32_t s = first; s < end; s++) {
        if (s >= SL_STAGE_SECTORS || !(p->bits[s / 8] & (1u << (s % 8)))) {
            return false;
        }
    }
    return true;
}

bool sl_part_present(const sl_manifest_t *m, const sl_presence_t *p, uint32_t region,
                     uint32_t part)
{
    if (region >= m->count) {
        return false;
    }
    const sl_region_t *r = &m->regions[region];
    if (part >= sl_part_count(r)) {
        return false;
    }
    uint32_t off = part * SL_PART_SIZE;
    uint32_t len = r->length - off < SL_PART_SIZE ? r->length - off : SL_PART_SIZE;
    return sectors_present(p, r->stage + off, len);
}

bool sl_part_mark(sl_manifest_t *m, sl_presence_t *p, uint32_t region, uint32_t stage_addr,
                  uint32_t len, bool present)
{
    sl_region_t *r = &m->regions[region];
    sl_presence_set(p, stage_addr, len, present);
    uint32_t was = r->flags & SL_FLAG_PRESENT;
    uint32_t now = sectors_present(p, r->stage, r->length) ? SL_FLAG_PRESENT : 0;
    r->flags = (r->flags & ~SL_FLAG_PRESENT) | now;
    return was != now;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool sl_hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_digit(hex[2 * i]);
        int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

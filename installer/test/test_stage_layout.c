// Host tests for installer/main/stage_layout.c (installer/SPEC.md, Tests 1).
//
//   test_stage_layout [STAGE_FILE IMAGE_FILE]...
//
// Each pair is a stage file from tools/make_stage.py and the image it was built
// from: the header decodes and the flat image read through it is the image,
// padded with 0xFF to 4 KB.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "stage_layout.h"

static sl_region_t region(uint32_t target, uint32_t length, int erase, uint8_t tag)
{
    sl_region_t r;
    memset(&r, 0, sizeof(r));
    r.target = target;
    r.length = length;
    r.flags = erase ? SL_FLAG_ERASE : 0;
    memset(r.sha256, tag, sizeof(r.sha256));
    return r;
}

// The Havoc Radio install image: bootloader, partition table, OTA data (all
// 0xFF, so erase-only) and a 684 KB app, gaps erase-only.
static void havoc_manifest(sl_manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    m->regions[0] = region(0x0, 0x3000, 0, 1);
    m->regions[1] = region(0x3000, 0x5000, 1, 2);
    m->regions[2] = region(0x8000, 0x1000, 0, 3);
    m->regions[3] = region(0x9000, 0x7000, 1, 4);
    m->regions[4] = region(0x10000, 171 * 4096, 0, 5);
    m->count = 5;
    sl_set_label(m, "Havoc Radio 0.1");
}

static void test_layout(void)
{
    sl_manifest_t m;
    havoc_manifest(&m);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
    CHECK(m.regions[0].stage == SL_HEADER_SIZE);
    CHECK(m.regions[1].stage == 0);
    CHECK(m.regions[2].stage == SL_HEADER_SIZE + 0x3000);
    CHECK(m.regions[4].stage == SL_HEADER_SIZE + 0x4000);
    CHECK(sl_part_count(&m.regions[0]) == 1);
    CHECK(sl_part_count(&m.regions[1]) == 0);
    CHECK(sl_part_count(&m.regions[4]) == 11);  // 684 KB = 10 full parts + 44 KB
    CHECK(sl_total_length(&m) == 0x10000 + 171 * 4096);
    CHECK(sl_data_regions(&m) == 3);

    // Data exactly filling the stage after its header fits; one sector more does not.
    memset(&m, 0, sizeof(m));
    m.regions[0] = region(0x0, SL_STAGE_MAX - SL_HEADER_SIZE, 0, 1);
    m.regions[1] = region(SL_STAGE_MAX, 0x200000, 1, 2);  // erase regions use no stage
    m.count = 2;
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
    m.regions[1].flags = 0;
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_TOO_BIG);
    m.regions[0].length = SL_STAGE_MAX;
    m.count = 1;
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_TOO_BIG);
    // A smaller stage partition is honoured.
    m.regions[0].length = 0x10000;
    CHECK(sl_layout(&m, 0x10000) == SL_ERR_TOO_BIG);
    CHECK(sl_layout(&m, 0x11000) == SL_OK);
}

static void test_validation(void)
{
    sl_manifest_t m;
    memset(&m, 0, sizeof(m));
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_COUNT);
    m.count = SL_MAX_REGIONS + 1;
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_COUNT);

    // 64 regions are allowed.
    m.count = SL_MAX_REGIONS;
    for (uint32_t i = 0; i < SL_MAX_REGIONS; i++) {
        m.regions[i] = region(i * 0x2000, 0x1000, (int)(i % 2), (uint8_t)i);
    }
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);

    m.count = 1;
    m.regions[0] = region(0x100, 0x1000, 0, 1);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_ALIGN);
    m.regions[0] = region(0x1000, 0x800, 0, 1);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_ALIGN);
    m.regions[0] = region(0x1000, 0, 1, 1);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_ALIGN);
    m.regions[0] = region(SL_TARGET_MAX - 0x1000, 0x2000, 1, 1);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_RANGE);
    m.regions[0] = region(0xFFFFF000u, 0x2000, 1, 1);  // would wrap
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_RANGE);
    m.regions[0] = region(SL_TARGET_MAX - 0x1000, 0x1000, 1, 1);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);

    // Overlap in either order; touching regions are fine.
    m.count = 2;
    m.regions[0] = region(0x10000, 0x2000, 0, 1);
    m.regions[1] = region(0x11000, 0x1000, 1, 2);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_OVERLAP);
    m.regions[1] = region(0xf000, 0x2000, 1, 2);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_ERR_OVERLAP);
    m.regions[1] = region(0x12000, 0x1000, 1, 2);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
    m.regions[1] = region(0xf000, 0x1000, 1, 2);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
}

static void test_equality(void)
{
    sl_manifest_t a, b;
    havoc_manifest(&a);
    havoc_manifest(&b);
    CHECK(sl_layout(&a, SL_STAGE_MAX) == SL_OK);
    CHECK(sl_manifest_equal(&a, &b));  // stage offsets are not compared
    b.regions[4].sha256[31] ^= 1;
    CHECK(!sl_manifest_equal(&a, &b));
    havoc_manifest(&b);
    b.regions[4].length -= SL_SECTOR_SIZE;
    CHECK(!sl_manifest_equal(&a, &b));
    havoc_manifest(&b);
    b.regions[2].target = 0x9000;
    CHECK(!sl_manifest_equal(&a, &b));
    havoc_manifest(&b);
    b.regions[1].flags = 0;
    CHECK(!sl_manifest_equal(&a, &b));
    havoc_manifest(&b);
    b.count = 4;
    CHECK(!sl_manifest_equal(&a, &b));
    havoc_manifest(&b);
    b.regions[2].flags |= SL_FLAG_PRESENT;  // presence does not count
    sl_set_label(&b, "other");               // nor does the label
    CHECK(sl_manifest_equal(&a, &b));
}

static void test_label(void)
{
    sl_manifest_t m;
    memset(&m, 0, sizeof(m));
    sl_set_label(&m, "havoc-radio-install.bin");
    CHECK(strcmp(m.label, "havoc-radio-install.bin") == 0);
    sl_set_label(&m, "0123456789012345678901234567890123456789");
    CHECK(strlen(m.label) == SL_LABEL_MAX);
    // 31 ASCII bytes and a 3-byte character: the character does not fit and goes.
    sl_set_label(&m, "0123456789012345678901234567890\xe6\x94\xb6");
    CHECK(strcmp(m.label, "0123456789012345678901234567890") == 0);
    sl_set_label(&m, "012345678901234567890123456789\xe6\x94\xb6");  // 33 bytes
    CHECK(strlen(m.label) == 30);
    sl_set_label(&m, "01234567890123456789012345678\xe6\x94\xb6");  // exactly 32
    CHECK(strlen(m.label) == 32);
}

static void test_header(void)
{
    static uint8_t sec[SL_HEADER_SIZE];
    sl_manifest_t m, d;
    havoc_manifest(&m);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
    m.regions[2].flags |= SL_FLAG_PRESENT;
    sl_header_encode(&m, sec);
    CHECK(memcmp(sec, "HVRI", 4) == 0);
    CHECK(sec[4] == 1 && sec[8] == 5);
    CHECK(sec[12] == 0x00 && sec[13] == 0xB0 && sec[14] == 0x0B && sec[15] == 0x00);  // 0xbb000
    CHECK(SL_HEADER_CRC_AT == 3120);

    // Round trip.
    CHECK(sl_header_decode(sec, &d, SL_STAGE_MAX));
    CHECK(d.count == 5 && strcmp(d.label, "Havoc Radio 0.1") == 0);
    CHECK(sl_manifest_equal(&m, &d));
    for (uint32_t i = 0; i < 5; i++) {
        CHECK(d.regions[i].stage == m.regions[i].stage && d.regions[i].flags == m.regions[i].flags);
    }
    CHECK(sl_flagged_regions(&d) == 1 && !sl_all_flagged(&d));
    d.regions[0].flags |= SL_FLAG_PRESENT;
    d.regions[4].flags |= SL_FLAG_PRESENT;
    CHECK(sl_all_flagged(&d));

    // A 32-byte label has no NUL in the header and still decodes.
    sl_set_label(&m, "0123456789012345678901234567890123");
    sl_header_encode(&m, sec);
    CHECK(sl_header_decode(sec, &d, SL_STAGE_MAX) && strlen(d.label) == 32);

    // Every corruption makes the header invalid, and the manifest empty.
    static const struct {
        uint32_t at;
        uint8_t xor;
    } bad[] = {
        {0, 1},                         // magic
        {4, 2},                         // version
        {12, 1},                        // total length
        {20, 1},                        // label (CRC)
        {SL_HEADER_REGIONS_AT + 2 * 48 + 8, 0x10},  // a stage offset
        {SL_HEADER_CRC_AT, 1},          // the CRC itself
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        sl_header_encode(&m, sec);
        sec[bad[k].at] ^= bad[k].xor;
        CHECK(!sl_header_decode(sec, &d, SL_STAGE_MAX));
        CHECK(d.count == 0);
    }
    // Field errors that carry a valid CRC: wrong total, layout, flags, count.
    struct {
        uint32_t at;
        uint32_t value;
    } fields[] = {
        {12, 0x1000},                                // total length
        {SL_HEADER_REGIONS_AT + 2 * 48 + 8, 0x2000},  // stage offset of region 2
        {SL_HEADER_REGIONS_AT + 0 * 48 + 12, 0x4},    // unknown flag
        {SL_HEADER_REGIONS_AT + 1 * 48 + 12, 0x3},    // erase-only and present
        {SL_HEADER_REGIONS_AT + 0 * 48 + 0, 0x800},   // unaligned target
        {8, 0},                                      // no regions
        {8, SL_MAX_REGIONS + 1},                     // too many
    };
    for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) {
        sl_header_encode(&m, sec);
        uint8_t *p = sec + fields[k].at;
        p[0] = (uint8_t)fields[k].value;
        p[1] = (uint8_t)(fields[k].value >> 8);
        p[2] = (uint8_t)(fields[k].value >> 16);
        p[3] = (uint8_t)(fields[k].value >> 24);
        uint32_t crc = sl_crc32(sec, SL_HEADER_CRC_AT);
        memcpy(sec + SL_HEADER_CRC_AT, &crc, 4);  // the host is little-endian, like the C3
        CHECK(!sl_header_decode(sec, &d, SL_STAGE_MAX));
    }
    // An erased sector is no header.
    memset(sec, 0xFF, sizeof(sec));
    CHECK(!sl_header_decode(sec, &d, SL_STAGE_MAX));
    // A header whose data no longer fits a smaller stage is invalid.
    sl_header_encode(&m, sec);
    CHECK(!sl_header_decode(sec, &d, 0x10000));

    // CRC-32 check value.
    CHECK(sl_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
}

static void test_flat(void)
{
    sl_manifest_t m;
    havoc_manifest(&m);
    m.regions[1] = region(0x4000, 0x4000, 1, 2);  // a gap at 0x3000..0x4000
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
    sl_span_t s = sl_flat_span(&m, 0, 0x10000);
    CHECK(s.len == 0x3000 && !s.fill && s.stage == SL_HEADER_SIZE);
    s = sl_flat_span(&m, 0x1800, 0x400);
    CHECK(s.len == 0x400 && !s.fill && s.stage == SL_HEADER_SIZE + 0x1800);
    s = sl_flat_span(&m, 0x3000, 0x10000);  // the gap
    CHECK(s.len == 0x1000 && s.fill);
    s = sl_flat_span(&m, 0x4000, 0x10000);  // erase-only
    CHECK(s.len == 0x4000 && s.fill);
    s = sl_flat_span(&m, 0x8000, 0x10000);
    CHECK(s.len == 0x1000 && !s.fill && s.stage == SL_HEADER_SIZE + 0x3000);
    s = sl_flat_span(&m, 0x10000 + 171 * 4096 - 1, 0x10000);
    CHECK(s.len == 1 && !s.fill);
    s = sl_flat_span(&m, 0x10000 + 171 * 4096, 0x10000);
    CHECK(s.len == 0);

    // A manifest that does not start at 0x0 reads 0xFF up to its first region,
    // in any order.
    memset(&m, 0, sizeof(m));
    m.regions[0] = region(0x20000, 0x1000, 0, 1);
    m.regions[1] = region(0x10000, 0x1000, 0, 2);
    m.count = 2;
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
    s = sl_flat_span(&m, 0, 0x100000);
    CHECK(s.len == 0x10000 && s.fill);
    s = sl_flat_span(&m, 0x11000, 0x100000);
    CHECK(s.len == 0xF000 && s.fill);
    s = sl_flat_span(&m, 0x10000, 0x100000);
    CHECK(s.len == 0x1000 && !s.fill && s.stage == SL_HEADER_SIZE + 0x1000);
}

static void test_parts(void)
{
    sl_manifest_t m;
    sl_presence_t p;
    havoc_manifest(&m);
    CHECK(sl_layout(&m, SL_STAGE_MAX) == SL_OK);
    sl_presence_clear(&p);

    uint32_t addr = 0;
    CHECK(sl_part_check(&m, 5, 0, 0x1000, &addr) == SL_ERR_REGION);
    CHECK(sl_part_check(&m, 1, 0, 0x1000, &addr) == SL_ERR_ERASE);
    CHECK(sl_part_check(&m, 4, 0x800, 0x1000, &addr) == SL_ERR_OFFSET);
    CHECK(sl_part_check(&m, 4, 0, 0, &addr) == SL_ERR_SIZE);
    CHECK(sl_part_check(&m, 4, 0, SL_PART_SIZE + SL_SECTOR_SIZE, &addr) == SL_ERR_SIZE);
    CHECK(sl_part_check(&m, 4, 0, 0x1800, &addr) == SL_ERR_SIZE);
    CHECK(sl_part_check(&m, 0, 0x2000, 0x2000, &addr) == SL_ERR_SIZE);  // past region end
    CHECK(sl_part_check(&m, 0, 0x3000, 0x1000, &addr) == SL_ERR_SIZE);
    CHECK(sl_part_check(&m, 4, 0xFFFFF000u, 0x2000, &addr) == SL_ERR_SIZE);
    CHECK(sl_part_check(&m, 4, 0x10000, SL_PART_SIZE, &addr) == SL_OK);
    CHECK(addr == m.regions[4].stage + 0x10000);

    // Upload every part of region 4 except the last; the flag comes with the 44 KB tail.
    for (uint32_t k = 0; k < 10; k++) {
        CHECK(sl_part_check(&m, 4, k * SL_PART_SIZE, SL_PART_SIZE, &addr) == SL_OK);
        CHECK(!sl_part_mark(&m, &p, 4, addr, SL_PART_SIZE, true));
        CHECK(sl_part_present(&m, &p, 4, k));
    }
    CHECK(!sl_part_present(&m, &p, 4, 10));
    CHECK(!sl_part_present(&m, &p, 4, 11));  // no such part
    CHECK(!(m.regions[4].flags & SL_FLAG_PRESENT));
    CHECK(sl_part_check(&m, 4, 10 * SL_PART_SIZE, 11 * SL_SECTOR_SIZE, &addr) == SL_OK);
    CHECK(sl_part_mark(&m, &p, 4, addr, 11 * SL_SECTOR_SIZE, true));  // flag changed
    CHECK(sl_part_present(&m, &p, 4, 10));
    CHECK(m.regions[4].flags & SL_FLAG_PRESENT);
    CHECK(sl_flagged_regions(&m) == 1);
    // Neighbouring regions are untouched.
    CHECK(!sl_part_present(&m, &p, 2, 0));
    CHECK(!sl_part_present(&m, &p, 0, 0));

    // A partial part marks only its sectors; the part is present once all are.
    CHECK(sl_part_check(&m, 0, 0, 0x1000, &addr) == SL_OK);
    CHECK(!sl_part_mark(&m, &p, 0, addr, 0x1000, true));
    CHECK(!sl_part_present(&m, &p, 0, 0));
    CHECK(sl_part_check(&m, 0, 0x1000, 0x2000, &addr) == SL_OK);
    CHECK(sl_part_mark(&m, &p, 0, addr, 0x2000, true));
    CHECK(sl_part_present(&m, &p, 0, 0));
    CHECK(!sl_all_flagged(&m));
    CHECK(sl_part_mark(&m, &p, 2, m.regions[2].stage, 0x1000, true));
    CHECK(sl_all_flagged(&m));

    // A part written again clears its sectors first: the region loses its flag
    // (the header must be written) and the other parts stay present.
    CHECK(sl_part_mark(&m, &p, 4, m.regions[4].stage + 0x20000, SL_PART_SIZE, false));
    CHECK(!sl_part_present(&m, &p, 4, 2));
    CHECK(sl_part_present(&m, &p, 4, 1));
    CHECK(sl_part_present(&m, &p, 4, 3));
    CHECK(!sl_all_flagged(&m));
    CHECK(!sl_part_mark(&m, &p, 4, m.regions[4].stage + 0x20000, SL_PART_SIZE, false));

    // After a reboot, presence comes from the header flags: complete regions
    // keep their parts, a region in progress starts over.
    sl_presence_from_flags(&m, &p);
    CHECK(sl_part_present(&m, &p, 0, 0));
    CHECK(sl_part_present(&m, &p, 2, 0));
    CHECK(!sl_part_present(&m, &p, 4, 1));

    // Presence past the bitmap is ignored, not written out of bounds.
    sl_presence_set(&p, SL_STAGE_MAX - SL_SECTOR_SIZE, 4 * SL_SECTOR_SIZE, true);
    CHECK(p.bits[sizeof(p.bits) - 1] & 0x80);
}

static void test_hex(void)
{
    uint8_t out[4];
    CHECK(sl_hex_decode("00a1FfE9", out, 4));
    CHECK(out[0] == 0x00 && out[1] == 0xa1 && out[2] == 0xff && out[3] == 0xe9);
    CHECK(!sl_hex_decode("00a1FfE", out, 4));
    CHECK(!sl_hex_decode("00a1FfE90", out, 4));
    CHECK(!sl_hex_decode("00a1FgE9", out, 4));
}

static uint8_t *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    if (f && fseek(f, 0, SEEK_END) == 0 && (*len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)*len);
        if (buf && fread(buf, 1, (size_t)*len, f) != (size_t)*len) {
            free(buf);
            buf = NULL;
        }
    }
    if (f) {
        fclose(f);
    }
    return buf;
}

// A stage file from tools/make_stage.py reads back as its image.
static void test_stage_file(const char *stage_path, const char *image_path)
{
    long slen = 0, ilen = 0;
    uint8_t *stage = read_file(stage_path, &slen);
    uint8_t *image = read_file(image_path, &ilen);
    CHECK(stage != NULL && image != NULL && slen >= (long)SL_HEADER_SIZE);
    if (stage == NULL || image == NULL || slen < (long)SL_HEADER_SIZE) {
        free(stage);
        free(image);
        return;
    }
    sl_manifest_t m;
    CHECK(sl_header_decode(stage, &m, SL_STAGE_MAX));
    CHECK(sl_all_flagged(&m));
    uint32_t total = sl_total_length(&m);
    CHECK(total == (uint32_t)((ilen + SL_SECTOR_SIZE - 1) / SL_SECTOR_SIZE * SL_SECTOR_SIZE));
    uint32_t diffs = 0;
    for (uint32_t off = 0; off < total;) {
        sl_span_t s = sl_flat_span(&m, off, 0x10000);
        if (s.len == 0 || (!s.fill && (long)(s.stage + s.len) > slen)) {
            diffs++;
            break;
        }
        for (uint32_t i = 0; i < s.len; i++) {
            uint8_t want = off + i < (uint32_t)ilen ? image[off + i] : 0xFF;
            uint8_t got = s.fill ? 0xFF : stage[s.stage + i];
            diffs += want != got;
        }
        off += s.len;
    }
    CHECK(diffs == 0);
    printf("%s: \"%s\", %u regions (%u data), %u bytes, flat image %s\n", stage_path, m.label, m.count,
           sl_data_regions(&m), total, diffs ? "DIFFERS" : "identical");
    free(stage);
    free(image);
}

int main(int argc, char **argv)
{
    test_layout();
    test_validation();
    test_equality();
    test_label();
    test_header();
    test_flat();
    test_parts();
    test_hex();
    for (int i = 1; i + 1 < argc; i += 2) {
        test_stage_file(argv[i], argv[i + 1]);
    }
    return check_report("stage_layout");
}

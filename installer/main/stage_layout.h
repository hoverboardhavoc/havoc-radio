// The carried image in the stage partition: region layout, the stage header
// and part bookkeeping (installer/SPEC.md, "The carried image").
//
// Plain C with no ESP-IDF dependencies, so it builds and is tested on the host.
// The HTTP handlers, the stage partition I/O and the flash job in stage.c and
// flash_job.c wrap it, and the host program serves a stage file with it.
//
// A manifest lists the regions of the image from target offset 0x0. A data
// region is staged in the stage partition; an erase-only region has no staged
// data. Sector 0 of the stage is the header; data regions follow back to back
// at 4 KB-aligned stage offsets in manifest order. The header keeps one present
// flag per region. While an upload runs, presence is tracked per 4 KB stage
// sector in RAM; a 64 KB part of a region is present when all of its sectors
// are, and a region gets its present flag once all of its sectors are.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_SECTOR_SIZE 4096u
#define SL_PART_SIZE 65536u
#define SL_MAX_REGIONS 64u
// The stage partition size: the presence bitmap covers this much. The build
// passes it from installer/partitions.csv (tools/make_stage.py --size).
#ifndef SL_STAGE_MAX
#error "SL_STAGE_MAX: the stage partition size, from installer/tools/make_stage.py --size"
#endif
#define SL_STAGE_SECTORS (SL_STAGE_MAX / SL_SECTOR_SIZE)
// The ESP8266 maps at most 16 MB of SPI flash.
#define SL_TARGET_MAX (16u * 1024u * 1024u)

// Stage header: sector 0 of the stage, little-endian.
#define SL_HEADER_SIZE SL_SECTOR_SIZE
#define SL_HEADER_MAGIC "HVRI"
#define SL_HEADER_VERSION 1u
#define SL_LABEL_MAX 32u
#define SL_HEADER_REGIONS_AT 48u
#define SL_HEADER_REGION_SIZE 48u
#define SL_HEADER_CRC_AT (SL_HEADER_REGIONS_AT + SL_MAX_REGIONS * SL_HEADER_REGION_SIZE)

#define SL_FLAG_ERASE 0x1u    // erase-only region
#define SL_FLAG_PRESENT 0x2u  // data region stored completely

typedef struct {
    uint32_t target;    // target flash offset, 4 KB-aligned
    uint32_t length;    // bytes, a multiple of 4 KB
    uint32_t stage;     // stage offset, set by sl_layout() (data regions only)
    uint32_t flags;     // SL_FLAG_*
    uint8_t sha256[32]; // of the data, or of the 0xFF run
} sl_region_t;

typedef struct {
    char label[SL_LABEL_MAX + 1];  // UTF-8, NUL-terminated
    uint32_t count;                // 0: no image
    sl_region_t regions[SL_MAX_REGIONS];
} sl_manifest_t;

typedef struct {
    uint8_t bits[SL_STAGE_SECTORS / 8];
} sl_presence_t;

typedef enum {
    SL_OK = 0,
    SL_ERR_COUNT,     // no regions, or more than SL_MAX_REGIONS
    SL_ERR_ALIGN,     // target or length not 4 KB-aligned, or zero length
    SL_ERR_RANGE,     // region runs past SL_TARGET_MAX
    SL_ERR_OVERLAP,   // two regions overlap at the target
    SL_ERR_TOO_BIG,   // data regions do not fit in the stage
    SL_ERR_REGION,    // part request names a missing region
    SL_ERR_ERASE,     // part request names an erase-only region
    SL_ERR_OFFSET,    // part offset not 4 KB-aligned
    SL_ERR_SIZE,      // part empty, over 64 KB, not whole sectors, or past the region end
} sl_err_t;

const char *sl_err_str(sl_err_t err);

static inline bool sl_is_erase(const sl_region_t *r) { return (r->flags & SL_FLAG_ERASE) != 0; }

// Validate the regions and assign each data region its stage offset, from
// SL_HEADER_SIZE on. stage_size is the stage partition size (at most
// SL_STAGE_MAX).
sl_err_t sl_layout(sl_manifest_t *m, uint32_t stage_size);

// True when both manifests list the same regions (target, length, erase flag,
// sha256) in the same order. Stage offsets follow from those; the label and the
// present flags do not count.
bool sl_manifest_equal(const sl_manifest_t *a, const sl_manifest_t *b);

// Copy label into m->label, cut at SL_LABEL_MAX bytes on a UTF-8 character
// boundary.
void sl_set_label(sl_manifest_t *m, const char *label);

// Encode the header sector (SL_HEADER_SIZE bytes) of a laid-out manifest.
void sl_header_encode(const sl_manifest_t *m, uint8_t *out);
// Decode a header sector. Returns false, with m->count 0, when the magic,
// version, region count, CRC, flags, layout or total length is wrong.
bool sl_header_decode(const uint8_t *in, sl_manifest_t *m, uint32_t stage_size);

// One past the last target byte of any region: the length of the flat image.
uint32_t sl_total_length(const sl_manifest_t *m);
// Every data region has its present flag (and there is at least one region).
bool sl_all_flagged(const sl_manifest_t *m);
// Number of data regions, and of those with the present flag.
uint32_t sl_data_regions(const sl_manifest_t *m);
uint32_t sl_flagged_regions(const sl_manifest_t *m);

// The flat image reads the regions in target order: data regions from the
// stage, everything else (erase-only regions and gaps) as 0xFF. A span is a run
// of at most max bytes from offset that comes from one source.
typedef struct {
    uint32_t len;    // 0 at or past the end of the image
    bool fill;       // 0xFF bytes
    uint32_t stage;  // stage offset of the first byte, when !fill
} sl_span_t;
sl_span_t sl_flat_span(const sl_manifest_t *m, uint32_t offset, uint32_t max);

// Number of 64 KB parts of a region; 0 for an erase-only region.
uint32_t sl_part_count(const sl_region_t *r);

// Validate a part upload of len bytes at offset inside region i and return its
// stage address.
sl_err_t sl_part_check(const sl_manifest_t *m, uint32_t region, uint32_t offset,
                       uint32_t len, uint32_t *stage_addr);

void sl_presence_clear(sl_presence_t *p);
// Mark the stage sectors covering [stage_addr, stage_addr + len) present or missing.
void sl_presence_set(sl_presence_t *p, uint32_t stage_addr, uint32_t len, bool present);
// Presence as the header records it: every sector of each flagged region.
void sl_presence_from_flags(const sl_manifest_t *m, sl_presence_t *p);
bool sl_part_present(const sl_manifest_t *m, const sl_presence_t *p, uint32_t region,
                     uint32_t part);

// Mark a part of region i (len bytes at stage_addr) present or missing, and set
// the region's present flag to whether all of its sectors are present. Returns
// true when the flag changed, so the header needs writing.
bool sl_part_mark(sl_manifest_t *m, sl_presence_t *p, uint32_t region, uint32_t stage_addr,
                  uint32_t len, bool present);

// Decode exactly 2 * n hex digits into out. Returns false on a bad digit or
// length.
bool sl_hex_decode(const char *hex, uint8_t *out, size_t n);

// CRC-32 (IEEE 802.3, as zlib's crc32()).
uint32_t sl_crc32(const uint8_t *data, size_t len);

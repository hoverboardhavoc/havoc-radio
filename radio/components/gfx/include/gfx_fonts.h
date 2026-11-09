// Font and icon tables. The data is generated from ui/prototype.html by tools/gen_fonts.py.
#pragma once
#include <cstdint>

namespace gfx {

// One Pip glyph: `rows` rows (6, or 7 with a descender), bit i of a row is column i.
struct PipGlyph {
    uint16_t cp;
    uint8_t width;
    uint8_t rows;
    uint8_t bits[7];
};

// One Facet glyph: offset into kFacetData of [polyline count, (point count, x0, y0, x1, y1, ...)...].
struct FacetGlyph {
    char ch;
    uint16_t offset;
};

struct IconBits {
    uint8_t width;
    uint8_t rows;
    uint8_t bits[8];
};

extern const PipGlyph kPip[];  // sorted by code point
extern const int kPipCount;
extern const uint8_t kFacetData[];
extern const FacetGlyph kFacet[];
extern const int kFacetCount;
extern const IconBits kIcons[];  // indexed by gfx::Icon

}  // namespace gfx

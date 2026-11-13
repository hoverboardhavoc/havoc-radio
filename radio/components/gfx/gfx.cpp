#include "gfx.h"

#include <cstring>

namespace gfx {

void Frame::clear() { std::memset(buf, 0, sizeof buf); }

void Frame::px(int x, int y, int c) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint8_t& b = buf[x + (y >> 3) * W];
    const uint8_t m = static_cast<uint8_t>(1u << (y & 7));
    if (c)
        b |= m;
    else
        b &= static_cast<uint8_t>(~m);
}

bool Frame::get(int x, int y) const {
    if (x < 0 || x >= W || y < 0 || y >= H) return false;
    return (buf[x + (y >> 3) * W] >> (y & 7)) & 1;
}

void pbmRows(const uint8_t* fb, uint8_t* out) {
    for (int y = 0; y < H; y++)
        for (int b = 0; b < W / 8; b++) {
            uint8_t v = 0;
            for (int i = 0; i < 8; i++)
                v = static_cast<uint8_t>(v << 1 | ((fb[b * 8 + i + (y >> 3) * W] >> (y & 7)) & 1));
            out[y * (W / 8) + b] = v;
        }
}

void Frame::fill(int x, int y, int w, int h, int c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) px(i, j, c);
}

uint32_t nextCp(const char*& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    if (!*p) return 0;
    uint32_t cp;
    int extra;
    if (*p < 0x80) {
        cp = *p;
        extra = 0;
    } else if ((*p & 0xe0) == 0xc0) {
        cp = *p & 0x1f;
        extra = 1;
    } else if ((*p & 0xf0) == 0xe0) {
        cp = *p & 0x0f;
        extra = 2;
    } else {
        cp = *p & 0x07;
        extra = 3;
    }
    p++;
    for (int i = 0; i < extra && (*p & 0xc0) == 0x80; i++, p++) cp = (cp << 6) | (*p & 0x3f);
    s = reinterpret_cast<const char*>(p);
    return cp;
}

const PipGlyph* pipGlyph(uint32_t cp) {
    int lo = 0, hi = kPipCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (kPip[mid].cp == cp) return &kPip[mid];
        if (kPip[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

// Widths follow the prototype: a missing glyph measures as '?' but draws as a space.
int glyphW(uint32_t cp) {
    const PipGlyph* g = pipGlyph(cp);
    if (!g) g = pipGlyph('?');
    return g->width;
}

int textW(const char* s) {
    int w = 0;
    while (uint32_t cp = nextCp(s)) w += glyphW(cp) + 1;
    return w > 0 ? w - 1 : 0;
}

int text(Frame& f, int x, int y, const char* s, int c) {
    while (uint32_t cp = nextCp(s)) {
        const PipGlyph* g = pipGlyph(cp);
        if (!g) g = pipGlyph(' ');
        for (int j = 0; j < g->rows; j++)
            for (int i = 0; i < g->width; i++)
                if ((g->bits[j] >> i) & 1) f.px(x + i, y + 1 + j, c);
        x += g->width + 1;
    }
    return x;
}

void textR(Frame& f, int xr, int y, const char* s, int c) { text(f, xr - textW(s), y, s, c); }

void text2x(Frame& f, int x, int y, const char* s) {
    while (uint32_t cp = nextCp(s)) {
        const PipGlyph* g = pipGlyph(cp);
        if (!g) g = pipGlyph(' ');
        for (int j = 0; j < g->rows; j++)
            for (int i = 0; i < g->width; i++)
                if ((g->bits[j] >> i) & 1) f.fill(x + 2 * i, y + 2 * j, 2, 2, 1);
        x += 2 * g->width + 2;
    }
}

namespace {
constexpr int kFacetAdv = 13;
constexpr int kFacetDot = 5;

const uint8_t* facetGlyph(char ch) {
    for (int i = 0; i < kFacetCount; i++)
        if (kFacet[i].ch == ch) return &kFacetData[kFacet[i].offset];
    return nullptr;
}
}  // namespace

int bigW(const char* s) {
    int w = 0;
    for (; *s; s++) w += *s == '.' ? kFacetDot : kFacetAdv;
    return w - 3;
}

void penLine(Frame& f, int x0, int y0, int x1, int y1) {
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        f.fill(x0, y0, 2, 2, 1);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * e;
        if (e2 >= dy) {
            e += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            e += dx;
            y0 += sy;
        }
    }
}

void big(Frame& f, int x, int top, const char* s) {
    for (; *s; s++) {
        if (*s == '.') {
            f.fill(x + 1, top + 16, 2, 2, 1);
            x += kFacetDot;
            continue;
        }
        if (const uint8_t* p = facetGlyph(*s)) {
            int lines = *p++;
            while (lines--) {
                const int n = *p++;
                for (int i = 1; i < n; i++)
                    penLine(f, x + p[2 * i - 2], top + p[2 * i - 1], x + p[2 * i], top + p[2 * i + 1]);
                p += 2 * n;
            }
        }
        x += kFacetAdv;
    }
}

void icon(Frame& f, int x, int y, Icon i, int c) {
    const IconBits& b = kIcons[static_cast<int>(i)];
    for (int j = 0; j < b.rows; j++)
        for (int k = 0; k < b.width; k++)
            if ((b.bits[j] >> k) & 1) f.px(x + k, y + j, c);
}

}  // namespace gfx

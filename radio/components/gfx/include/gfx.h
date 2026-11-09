// 128x32 one-bit framebuffer and the drawing primitives of the UI.
#pragma once
#include <cstdint>

#include "gfx_fonts.h"

namespace gfx {

constexpr int W = 128;
constexpr int H = 32;
constexpr int kBytes = W * H / 8;

// Framebuffer in SSD1306 page order: byte x + (y / 8) * W, bit y % 8.
struct Frame {
    uint8_t buf[kBytes];

    void clear();
    void px(int x, int y, int c);
    bool get(int x, int y) const;
    void fill(int x, int y, int w, int h, int c);
};

enum class Icon : uint8_t { Presets, Settings, Volume, Muted };

// Decodes one UTF-8 code point and advances s. Returns 0 at the end of the string.
uint32_t nextCp(const char*& s);

const PipGlyph* pipGlyph(uint32_t cp);  // nullptr when the font has no glyph
int glyphW(uint32_t cp);
int textW(const char* s);
// Draws Pip text with the cell top at y (glyph rows start at y + 1). Returns the x after the last advance.
int text(Frame& f, int x, int y, const char* s, int c = 1);
void textR(Frame& f, int xr, int y, const char* s, int c = 1);
// Pip at 2x, top row at y.
void text2x(Frame& f, int x, int y, const char* s);

int bigW(const char* s);
void big(Frame& f, int x, int top, const char* s);
// Bresenham line drawn with the 2x2 Facet pen.
void penLine(Frame& f, int x0, int y0, int x1, int y1);

void icon(Frame& f, int x, int y, Icon i, int c = 1);

}  // namespace gfx

#include "mono_canvas.h"

#include <cstring>

namespace oled {

void MonoCanvas::clear(Ink ink) {
    std::memset(buf_, ink == Ink::On ? 0xFF : 0x00, kBufferBytes);
}

void MonoCanvas::pixel(int16_t x, int16_t y, Ink ink) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    const size_t idx = static_cast<size_t>(y >> 3) * kWidth + static_cast<size_t>(x);
    const uint8_t mask = static_cast<uint8_t>(1u << (y & 7));
    switch (ink) {
        case Ink::On:     buf_[idx] |= mask; break;
        case Ink::Off:    buf_[idx] = static_cast<uint8_t>(buf_[idx] & ~mask); break;
        case Ink::Invert: buf_[idx] = static_cast<uint8_t>(buf_[idx] ^ mask); break;
    }
}

bool MonoCanvas::get(int16_t x, int16_t y) const {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return false;
    const size_t idx = static_cast<size_t>(y >> 3) * kWidth + static_cast<size_t>(x);
    return (buf_[idx] & (1u << (y & 7))) != 0;
}

void MonoCanvas::hline(int16_t x, int16_t y, int16_t w, Ink ink) {
    for (int16_t i = 0; i < w; ++i) pixel(static_cast<int16_t>(x + i), y, ink);
}

void MonoCanvas::vline(int16_t x, int16_t y, int16_t h, Ink ink) {
    for (int16_t i = 0; i < h; ++i) pixel(x, static_cast<int16_t>(y + i), ink);
}

void MonoCanvas::rect(int16_t x, int16_t y, int16_t w, int16_t h, Ink ink) {
    if (w <= 0 || h <= 0) return;
    hline(x, y, w, ink);
    hline(x, static_cast<int16_t>(y + h - 1), w, ink);
    vline(x, y, h, ink);
    vline(static_cast<int16_t>(x + w - 1), y, h, ink);
}

void MonoCanvas::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, Ink ink) {
    for (int16_t row = 0; row < h; ++row)
        hline(x, static_cast<int16_t>(y + row), w, ink);
}

void MonoCanvas::glyph(int16_t x, int16_t baseline, const Glyph& g, const Font& f,
                       Ink ink) {
    if (g.width == 0 || g.height == 0) return;
    const uint8_t stride = static_cast<uint8_t>((g.width + 7) / 8);
    const uint8_t* src = f.bits + g.offset;
    for (uint8_t row = 0; row < g.height; ++row) {
        for (uint8_t colIdx = 0; colIdx < g.width; ++colIdx) {
            const uint8_t byte = src[row * stride + (colIdx >> 3)];
            if (byte & (0x80 >> (colIdx & 7))) {
                pixel(static_cast<int16_t>(x + g.xOffset + colIdx),
                      static_cast<int16_t>(baseline + g.yOffset + row), ink);
            }
        }
    }
}

int16_t MonoCanvas::text(int16_t x, int16_t y, const char* s, const Font& f, Ink ink) {
    if (!s) return 0;
    int16_t pen = x;
    for (const char* p = s; *p; ++p) {
        const Glyph* g = f.glyph(*p);
        if (!g) continue;
        glyph(pen, y, *g, f, ink);
        pen = static_cast<int16_t>(pen + g->advance);
    }
    return static_cast<int16_t>(pen - x);
}

int16_t MonoCanvas::textWidth(const char* s, const Font& f) const {
    if (!s) return 0;
    int16_t w = 0;
    for (const char* p = s; *p; ++p) {
        const Glyph* g = f.glyph(*p);
        if (g) w = static_cast<int16_t>(w + g->advance);
    }
    return w;
}

int16_t MonoCanvas::textClipped(int16_t x, int16_t y, int16_t maxWidth, const char* s,
                                const Font& f, Ink ink) {
    if (!s || maxWidth <= 0) return 0;
    if (textWidth(s, f) <= maxWidth) return text(x, y, s, f, ink);

    // Reserve room for the ellipsis before deciding what fits, so the marker
    // can never itself overflow the space -- the failure that makes a clipped
    // line look like a complete one.
    const int16_t dots = textWidth("..", f);
    int16_t pen = x;
    const int16_t limit = static_cast<int16_t>(x + maxWidth - dots);
    for (const char* p = s; *p; ++p) {
        const Glyph* g = f.glyph(*p);
        if (!g) continue;
        if (pen + g->advance > limit) break;
        glyph(pen, y, *g, f, ink);
        pen = static_cast<int16_t>(pen + g->advance);
    }
    pen = static_cast<int16_t>(pen + text(pen, y, "..", f, ink));
    return static_cast<int16_t>(pen - x);
}

int16_t MonoCanvas::textRight(int16_t x, int16_t w, int16_t y, const char* s,
                              const Font& f, Ink ink) {
    const int16_t tw = textWidth(s, f);
    return text(static_cast<int16_t>(x + w - tw), y, s, f, ink);
}

} // namespace oled
